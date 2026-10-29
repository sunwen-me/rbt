//! Physical-camera example components that bind stamped capture time to TF.
#![forbid(unsafe_code)]

use std::{
    fs,
    io::{self, BufRead, BufReader, Read, Write},
    path::PathBuf,
    process::{Child, Command, Stdio},
    sync::{Arc, Mutex, mpsc},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

use anyhow::{Context, anyhow, bail};
use async_trait::async_trait;
use rbt_compose::{ActivationCtx, Component};
use rbt_core::{Capability, CapabilityInstanceId, ComponentId, RequirementSpec};
use rbt_iox2::{SliceStream, WaitContext, WaitProgress, WaitToken};
use rbt_tf::{
    FrameId, LookupOptions, LookupPolicy, StaticExtrinsicsConfig, TfResourceConfig,
    TransformBuffer, Vec3, config::MAX_STATIC_CONFIG_BYTES,
};
use rbt_tf_transport::TransformActivationExt;
use rbt_time::{ClockKind, HostClock, Timestamp};
use rbt_time_stream::{StampedBytes, split_payload};
use rbt_types::StreamProfile;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

const V4L2_TIMESTAMP_MASK: u32 = 0x0000_e000;
const V4L2_TIMESTAMP_MONOTONIC: u32 = 0x0000_2000;
const V4L2_SOURCE_MASK: u32 = 0x0007_0000;
const V4L2_SOURCE_SOE: u32 = 0x0001_0000;
const V4L2_PIX_FMT_YUYV: u32 = u32::from_le_bytes(*b"YUYV");
const MAX_TRANSFORMS: usize = 64;
const MAX_FRAME_BYTES: usize = 16 * 1024 * 1024;
const MAX_CAMERA_CHILD_JSONL_RECORD_BYTES: usize = 64 * 1024;

/// Capability role for the physical StampedBytes camera.
pub struct CameraStream;
impl Capability for CameraStream {
    const TYPE_NAME: &'static str = "StampedBytesCamera";
}

/// Capability role for the static TF full-update stream.
pub struct StaticTfUpdate;
impl Capability for StaticTfUpdate {
    const TYPE_NAME: &'static str = "StaticTfUpdate";
}

/// Capability role for the static TF snapshot RPC paired with [`StaticTfUpdate`].
pub struct StaticTfSnapshot;
impl Capability for StaticTfSnapshot {
    const TYPE_NAME: &'static str = "StaticTfSnapshot";
}

#[derive(Clone, Debug)]
pub struct CameraConfig {
    pub component_id: String,
    pub device: PathBuf,
    pub camera_executable: PathBuf,
    pub frame_id: String,
    pub duration_seconds: u64,
}

#[derive(Clone, Debug)]
pub struct CalibrationConfig {
    pub component_id: String,
    pub path: PathBuf,
}

#[derive(Clone, Debug)]
pub struct PerceptionConfig {
    pub component_id: String,
    pub frame_id: String,
    pub duration_seconds: u64,
}

pub struct CameraComponent {
    config: CameraConfig,
    frame_id: FrameId,
}

pub struct CalibrationComponent {
    component_id: ComponentId,
    extrinsics: StaticExtrinsicsConfig,
}

pub struct PerceptionComponent {
    config: PerceptionConfig,
    frame_id: FrameId,
}

struct CameraFrame {
    sequence: u64,
    frame_id: FrameId,
    width: u32,
    height: u32,
    stride: u32,
    raw_driver_ns: i64,
    driver_flags: u32,
    driver_session_generation: u64,
    capture: Option<Timestamp>,
    checksum: u64,
}

struct DecodedCameraV2<'a> {
    frame_id: FrameId,
    width: u32,
    height: u32,
    stride: u32,
    raw_driver_ns: i64,
    driver_flags: u32,
    driver_session_generation: u64,
    pixels: &'a [u8],
}

#[async_trait]
impl Component for CameraComponent {
    type Config = CameraConfig;
    type Error = anyhow::Error;

    async fn prepare(config: &Self::Config) -> anyhow::Result<Self> {
        if !config.device.is_absolute() || !config.camera_executable.is_absolute() {
            bail!("camera device and native executable must be explicit absolute paths");
        }
        if config.duration_seconds == 0 {
            bail!("camera duration must be positive");
        }
        let frame_id = FrameId::new(&config.frame_id).context("validate camera frame id")?;
        Ok(Self {
            config: config.clone(),
            frame_id,
        })
    }

    fn id(&self) -> ComponentId {
        ComponentId::from(self.config.component_id.as_str())
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> anyhow::Result<()> {
        let capability = CapabilityInstanceId::new();
        let service = capability.stream_service_name();
        let child = start_camera_child(&self.config, self.frame_id, &service, ctx.scope())?;

        let descriptor =
            SliceStream::<StampedBytes>::descriptor(service.clone(), StreamProfile::Sensor);
        ctx.provide::<CameraStream>(capability, descriptor);
        let monitor_child = Arc::clone(&child.child);
        ctx.spawn_critical_blocking(
            "physical-camera-child-monitor",
            move |cancellation| monitor_camera_child(monitor_child, cancellation),
            || {},
        );
        emit(json!({
            "event": "camera_ready",
            "device": self.config.device,
            "service": service,
            "frame_id": self.frame_id.as_str(),
            "capability_instance": capability,
            "child_pid": child.pid,
            "child_command": child.command,
            "format": "RBTVCAM2"
        }));
        Ok(())
    }
}

#[async_trait]
impl Component for CalibrationComponent {
    type Config = CalibrationConfig;
    type Error = anyhow::Error;

    async fn prepare(config: &Self::Config) -> anyhow::Result<Self> {
        let file = fs::File::open(&config.path).with_context(|| {
            format!(
                "open explicit static extrinsics config {}",
                config.path.display()
            )
        })?;
        let mut bytes = Vec::with_capacity(MAX_STATIC_CONFIG_BYTES.min(4096));
        file.take((MAX_STATIC_CONFIG_BYTES + 1) as u64)
            .read_to_end(&mut bytes)
            .context("read explicit static extrinsics config")?;
        if bytes.len() > MAX_STATIC_CONFIG_BYTES {
            bail!("static extrinsics config exceeds the 1 MiB input limit");
        }
        let source =
            String::from_utf8(bytes).context("static extrinsics config is not valid UTF-8")?;
        let extrinsics = StaticExtrinsicsConfig::from_toml(&source, MAX_TRANSFORMS)
            .context("validate static extrinsics before registering TF authority")?;
        Ok(Self {
            component_id: ComponentId::from(config.component_id.as_str()),
            extrinsics,
        })
    }

    fn id(&self) -> ComponentId {
        self.component_id.clone()
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> anyhow::Result<()> {
        let clock = HostClock::new().context("establish calibration HostClock")?;
        let effective_from = clock
            .monotonic_now()
            .context("read calibration effective time")?;
        let static_set = self
            .extrinsics
            .clone()
            .into_static_set(1, effective_from, None);
        let provided = ctx.provide_static_tf::<StaticTfUpdate, StaticTfSnapshot>(
            static_set,
            MAX_TRANSFORMS,
            StreamProfile::Control,
        )?;
        emit(json!({
            "event": "static_calibration_ready",
            "update_authority": provided.update_authority,
            "snapshot_capability": provided.snapshot_capability,
            "revision": 1,
            "effective_from_ns": effective_from.nanoseconds(),
            "effective_uncertainty_ns": null,
            "configured_edges": self.extrinsics.edges().len(),
            "calibration": "Configured extrinsic — calibration accuracy unverified"
        }));
        Ok(())
    }
}

#[async_trait]
impl Component for PerceptionComponent {
    type Config = PerceptionConfig;
    type Error = anyhow::Error;

    async fn prepare(config: &Self::Config) -> anyhow::Result<Self> {
        if config.duration_seconds == 0 {
            bail!("perception duration must be positive");
        }
        Ok(Self {
            config: config.clone(),
            frame_id: FrameId::new(&config.frame_id).context("validate perception frame id")?,
        })
    }

    fn id(&self) -> ComponentId {
        ComponentId::from(self.config.component_id.as_str())
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        vec![
            RequirementSpec::typed::<CameraStream>("camera"),
            RequirementSpec::typed::<StaticTfUpdate>("tf-update"),
            RequirementSpec::typed::<StaticTfSnapshot>("tf-snapshot"),
        ]
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> anyhow::Result<()> {
        let resources = TfResourceConfig::default();
        let buffer = TransformBuffer::new(
            resources,
            LookupPolicy {
                max_timestamp_uncertainty_ns: None,
                max_interpolation_gap: Duration::from_secs(1),
                allow_unknown_uncertainty: true,
            },
        )?;
        let tf_binding = ctx.require_static_tf::<StaticTfUpdate, StaticTfSnapshot>(
            "tf-update",
            "tf-snapshot",
            "camera-static-extrinsics",
            MAX_TRANSFORMS,
            buffer.clone(),
        )?;
        let camera = ctx.binding::<CameraStream>("camera")?;
        let descriptor = camera
            .try_stream()
            .ok_or_else(|| anyhow!("camera requirement is not a stream"))?;
        let subscriber = SliceStream::<StampedBytes>::subscriber(descriptor)?;
        let process = ctx.process_instance();
        let activation = ctx.activation();
        let wait = WaitContext::new(process, activation)?;
        let wait_wake = wait.wake_handle();
        ctx.scope().wake_on_shutdown(move || {
            let _ = wait_wake.wake();
        });

        let frame_id = self.frame_id;
        let buffer_for_task = buffer.clone();
        let authority = tf_binding.authority;
        let timeout = Duration::from_secs(self.config.duration_seconds);
        let service = descriptor.service_name().to_owned();
        emit(json!({
            "event": "perception_ready",
            "camera_service": service,
            "frame_id": frame_id.as_str(),
            "tf_authority": authority.physical(),
            "static_tf_snapshot": "subscribed_before_snapshot_request",
            "precision_policy": "best_effort_unknown_uncertainty",
            "physical_dynamic_tf": "NOT_RUN"
        }));
        ctx.spawn_background_blocking(
            "physical-camera-tf-perception",
            move |cancellation| {
                consume_camera(
                    subscriber,
                    wait,
                    cancellation,
                    buffer_for_task,
                    frame_id,
                    service,
                    timeout,
                )
            },
            || {},
        );
        Ok(())
    }
}

struct StartedCamera {
    child: Arc<Mutex<Option<Child>>>,
    pid: u32,
    command: Vec<String>,
}

fn start_camera_child(
    config: &CameraConfig,
    frame_id: FrameId,
    service: &str,
    scope: &mut rbt_compose::EffectScope,
) -> anyhow::Result<StartedCamera> {
    let command = vec![
        config.camera_executable.to_string_lossy().into_owned(),
        "--device".to_owned(),
        config.device.to_string_lossy().into_owned(),
        "--service".to_owned(),
        service.to_owned(),
        "--duration-sec".to_owned(),
        config.duration_seconds.to_string(),
        "--frame-id".to_owned(),
        frame_id.as_str().to_owned(),
    ];
    let child = Command::new(&config.camera_executable)
        .args(&command[1..])
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .with_context(|| {
            format!(
                "start native camera executable {}",
                config.camera_executable.display()
            )
        })?;
    let pid = child.id();
    let child = Arc::new(Mutex::new(Some(child)));
    let reader = Arc::new(Mutex::new(None::<JoinHandle<()>>));
    {
        let child_for_cleanup = Arc::clone(&child);
        let reader_for_cleanup = Arc::clone(&reader);
        scope.defer_sync(move || stop_camera_child(child_for_cleanup, reader_for_cleanup));
    }
    let stdout = child
        .lock()
        .map_err(|_| anyhow!("camera child state was poisoned"))?
        .as_mut()
        .ok_or_else(|| anyhow!("native camera ended before JSONL readiness"))?
        .stdout
        .take()
        .ok_or_else(|| anyhow!("native camera stdout was not piped"))?;

    let (ready_tx, ready_rx) = mpsc::sync_channel::<Value>(1);
    let reader_thread = thread::Builder::new()
        .name("tf-camera-child-jsonl".to_owned())
        .spawn(move || {
            let mut stdout = BufReader::new(stdout);
            loop {
                match read_camera_jsonl_record(&mut stdout) {
                    Ok(CameraJsonlRecord::Complete(mut record)) => {
                        record.pop(); // The reader returns complete newline-terminated records.
                        if record.last() == Some(&b'\r') {
                            record.pop();
                        }
                        let Ok(line) = std::str::from_utf8(&record) else {
                            continue;
                        };
                        if let Ok(value) = serde_json::from_str::<Value>(line)
                            && value.get("event").and_then(Value::as_str) == Some("device")
                        {
                            let _ = ready_tx.try_send(value);
                        }
                        println!("{line}");
                        let _ = std::io::stdout().flush();
                    }
                    Ok(CameraJsonlRecord::Oversized) => {
                        eprintln!(
                            "ignored native camera JSONL record exceeding {MAX_CAMERA_CHILD_JSONL_RECORD_BYTES} bytes"
                        );
                    }
                    Ok(CameraJsonlRecord::Incomplete | CameraJsonlRecord::Eof) => break,
                    Err(error) => {
                        eprintln!("failed to read native camera JSONL: {error}");
                        break;
                    }
                }
            }
        })
        .context("start native camera JSONL reader")?;
    *reader
        .lock()
        .map_err(|_| anyhow!("camera reader state was poisoned"))? = Some(reader_thread);

    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match ready_rx.recv_timeout(Duration::from_millis(50)) {
            Ok(record) => {
                if record.get("device").and_then(Value::as_str)
                    != Some(config.device.to_string_lossy().as_ref())
                    || record.get("service").and_then(Value::as_str) != Some(service)
                    || record.get("application_format").and_then(Value::as_str) != Some("RBTVCAM2")
                    || record.get("frame_id").and_then(Value::as_str) != Some(frame_id.as_str())
                {
                    bail!(
                        "native camera readiness record does not match selected device, service, and frame"
                    );
                }
                return Ok(StartedCamera {
                    child,
                    pid,
                    command,
                });
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                bail!("native camera exited before bounded readiness")
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
        if Instant::now() >= deadline {
            bail!("native camera did not become ready within ten seconds");
        }
        let status = child
            .lock()
            .map_err(|_| anyhow!("camera child state was poisoned"))?
            .as_mut()
            .ok_or_else(|| anyhow!("native camera ended before readiness"))?
            .try_wait()
            .context("poll native camera readiness")?;
        if let Some(status) = status {
            bail!("native camera exited before readiness with {status}");
        }
    }
}

enum CameraJsonlRecord {
    Complete(Vec<u8>),
    Oversized,
    Incomplete,
    Eof,
}

fn read_camera_jsonl_record(reader: &mut impl BufRead) -> io::Result<CameraJsonlRecord> {
    let mut record = Vec::with_capacity(MAX_CAMERA_CHILD_JSONL_RECORD_BYTES.min(4096));
    loop {
        let (consumed, terminated, oversized) = {
            let available = reader.fill_buf()?;
            if available.is_empty() {
                return Ok(if record.is_empty() {
                    CameraJsonlRecord::Eof
                } else {
                    CameraJsonlRecord::Incomplete
                });
            }
            let newline = available.iter().position(|byte| *byte == b'\n');
            let consumed = newline.map_or(available.len(), |index| index + 1);
            let oversized =
                record.len().saturating_add(consumed) > MAX_CAMERA_CHILD_JSONL_RECORD_BYTES;
            if !oversized {
                record.extend_from_slice(&available[..consumed]);
            }
            (consumed, newline.is_some(), oversized)
        };
        reader.consume(consumed);
        if oversized {
            if !terminated {
                discard_camera_jsonl_record(reader)?;
            }
            return Ok(CameraJsonlRecord::Oversized);
        }
        if terminated {
            return Ok(CameraJsonlRecord::Complete(record));
        }
    }
}

fn discard_camera_jsonl_record(reader: &mut impl BufRead) -> io::Result<()> {
    loop {
        let (consumed, terminated) = {
            let available = reader.fill_buf()?;
            if available.is_empty() {
                return Ok(());
            }
            let newline = available.iter().position(|byte| *byte == b'\n');
            (
                newline.map_or(available.len(), |index| index + 1),
                newline.is_some(),
            )
        };
        reader.consume(consumed);
        if terminated {
            return Ok(());
        }
    }
}

fn monitor_camera_child(
    child: Arc<Mutex<Option<Child>>>,
    cancellation: CancellationToken,
) -> anyhow::Result<()> {
    while !cancellation.is_cancelled() {
        let status = child
            .lock()
            .map_err(|_| anyhow!("camera child state was poisoned"))?
            .as_mut()
            .ok_or_else(|| anyhow!("camera activation has ended"))?
            .try_wait()
            .context("monitor native camera child")?;
        if let Some(status) = status {
            bail!("native camera child exited unexpectedly with {status}");
        }
        thread::sleep(Duration::from_millis(100));
    }
    Ok(())
}

fn stop_camera_child(child: Arc<Mutex<Option<Child>>>, reader: Arc<Mutex<Option<JoinHandle<()>>>>) {
    let mut child_state = match child.lock() {
        Ok(state) => state,
        Err(poisoned) => {
            eprintln!("camera child lock poisoned during cleanup; forcing kill and reap");
            poisoned.into_inner()
        }
    };
    if let Some(mut process) = child_state.take() {
        match process.try_wait() {
            Ok(Some(_)) => {}
            Ok(None) => {
                if let Err(error) = process.kill() {
                    eprintln!("failed to kill native camera child: {error}");
                }
            }
            Err(error) => {
                eprintln!("failed to poll native camera child during cleanup: {error}");
                if let Err(kill_error) = process.kill() {
                    eprintln!("failed to kill native camera child after poll error: {kill_error}");
                }
            }
        }
        if let Err(error) = process.wait() {
            eprintln!("failed to reap native camera child: {error}");
        }
    }
    drop(child_state);
    let mut reader_state = match reader.lock() {
        Ok(state) => state,
        Err(poisoned) => {
            eprintln!("camera child reader lock poisoned during cleanup; joining reader");
            poisoned.into_inner()
        }
    };
    if let Some(reader) = reader_state.take()
        && reader.join().is_err()
    {
        eprintln!("native camera JSONL reader thread panicked");
    }
}

#[allow(clippy::too_many_arguments)]
fn consume_camera(
    subscriber: rbt_iox2::SliceSubscriber<StampedBytes>,
    mut wait: WaitContext,
    cancellation: CancellationToken,
    buffer: TransformBuffer,
    expected_frame: FrameId,
    service: String,
    duration: Duration,
) -> anyhow::Result<()> {
    let clock = HostClock::new().context("establish perception HostClock")?;
    let start = Instant::now();
    let deadline = start
        .checked_add(duration)
        .ok_or_else(|| anyhow!("perception duration exceeds steady-clock range"))?;
    emit(json!({
        "event": "tf_camera_ingestor_started",
        "service": service,
        "frame_id": expected_frame.as_str(),
        "host_clock_source": clock.monotonic_identity().source_id().to_hex(),
        "precision_policy": "best_effort_unknown_uncertainty",
        "physical_dynamic_tf": "NOT_RUN"
    }));
    let event_source = subscriber.event_source(WaitToken::new(1));
    let sources = [event_source];

    while !cancellation.is_cancelled() && Instant::now() < deadline {
        let remaining = deadline.saturating_duration_since(Instant::now());
        let timeout = remaining.min(Duration::from_millis(250));
        let outcome = wait.run_with_timeout(&sources, timeout, |_, budget| {
            let mut received = 0usize;
            while received < budget {
                let Some(sample) = subscriber.receive()? else {
                    break;
                };
                received += 1;
                let frame = inspect_sample(&sample, &clock, expected_frame)?;
                drop(sample);
                let borrowed = subscriber
                    .pressure()
                    .subscriber
                    .map_or(0, |pressure| pressure.borrowed_samples);
                emit(json!({
                    "event": "camera_sample_released",
                    "borrowed_samples": borrowed
                }));
                process_frame(frame, &buffer)?;
            }
            Ok::<_, anyhow::Error>(if received == 0 {
                WaitProgress::Empty
            } else {
                WaitProgress::Samples(received)
            })
        })?;
        if cancellation.is_cancelled() || wait.is_cancelled() || outcome.is_some() {
            break;
        }
    }

    let elapsed_ms = start.elapsed().as_millis() as u64;
    let pressure = subscriber.pressure();
    let borrowed = pressure
        .subscriber
        .map_or(0, |snapshot| snapshot.borrowed_samples);
    emit(json!({
        "event": "perception_summary",
        "actual_elapsed_ms": elapsed_ms,
        "borrowed_samples": borrowed,
        "physical_dynamic_tf": "NOT_RUN"
    }));
    if borrowed != 0 {
        bail!("camera subscriber retained a native sample borrow during cleanup");
    }
    Ok(())
}

fn inspect_sample(
    sample: &rbt_iox2::ReceivedSliceSample<StampedBytes>,
    host_clock: &HostClock,
    expected_frame: FrameId,
) -> anyhow::Result<CameraFrame> {
    let sequence = *sample.header();
    if sequence > u64::from(u32::MAX) {
        bail!("V4L2 sequence exceeds its u32 driver range");
    }
    let payload = sample.payload();
    let (timing, application) =
        split_payload(payload).context("decode StampedBytes timing prefix")?;
    let DecodedCameraV2 {
        frame_id: actual_frame,
        width,
        height,
        stride,
        raw_driver_ns,
        driver_flags,
        driver_session_generation,
        pixels,
    } = decode_camera_v2(application)?;
    if actual_frame != expected_frame {
        bail!("camera sample frame identity does not match the selected frame");
    }
    if timing.capture_uncertainty_ns().is_some() {
        bail!("camera sample unexpectedly claims a capture precision bound");
    }
    let received = timing.received();
    if received.clock().kind() != ClockKind::HostMonotonic
        || received.clock() != host_clock.monotonic_identity()
    {
        bail!("camera receipt time is not from this host monotonic identity");
    }
    let capture = timing.capture().copied();
    let capture_valid = if let Some(capture) = capture {
        if capture.clock().kind() != ClockKind::HostMonotonic
            || capture.clock() != received.clock()
            || capture.nanoseconds() != raw_driver_ns
            || !host_offset_is_zero(host_clock)
            || driver_flags & V4L2_TIMESTAMP_MASK != V4L2_TIMESTAMP_MONOTONIC
            || driver_flags & V4L2_SOURCE_MASK != V4L2_SOURCE_SOE
        {
            bail!("captured timestamp disagrees with the V2 V4L2 MONOTONIC+SOE metadata");
        }
        true
    } else {
        false
    };
    if !capture_valid
        && host_offset_is_zero(host_clock)
        && driver_flags & V4L2_TIMESTAMP_MASK == V4L2_TIMESTAMP_MONOTONIC
        && driver_flags & V4L2_SOURCE_MASK == V4L2_SOURCE_SOE
    {
        bail!("verified MONOTONIC+SOE camera sample omitted its capture timestamp");
    }
    let checksum = fnv1a64(pixels);
    Ok(CameraFrame {
        sequence,
        frame_id: actual_frame,
        width,
        height,
        stride,
        raw_driver_ns,
        driver_flags,
        driver_session_generation,
        capture,
        checksum,
    })
}

fn process_frame(frame: CameraFrame, buffer: &TransformBuffer) -> anyhow::Result<()> {
    let CameraFrame {
        sequence,
        frame_id: actual_frame,
        width,
        height,
        stride,
        raw_driver_ns,
        driver_flags,
        driver_session_generation,
        capture,
        checksum,
    } = frame;
    let lookup = if let Some(capture) = capture {
        let result = buffer.lookup_at_with(
            "base_link",
            actual_frame.as_str(),
            capture,
            LookupOptions {
                timestamp_uncertainty_ns: None,
                mappings: &[],
                now: None,
            },
        );
        match result {
            Ok(result) => {
                let point = Vec3::new(0.125, -0.25, 1.0)?;
                let round_trip = result
                    .transform
                    .inverse()?
                    .apply_point(result.transform.apply_point(point)?)?;
                let inverse_point_error_m = euclidean_distance(point, round_trip);
                let (tx, ty, tz) = result.transform.translation().xyz();
                let (qx, qy, qz, qw) = result.transform.rotation().xyzw();
                let sources = result
                    .sources
                    .iter()
                    .map(|source| {
                        json!({
                            "authority": source.authority.physical(),
                            "slot": source.authority.slot(),
                            "static_revision": source.static_revision,
                            "transform_epoch": source.transform_epoch,
                            "parent": source.parent.as_str(),
                            "child": source.child.as_str()
                        })
                    })
                    .collect::<Vec<_>>();
                let missing_world = world_lookup_status(buffer, capture, actual_frame)?;
                emit(json!({
                    "event": "tf_lookup",
                    "status": "ok",
                    "target": "base_link",
                    "source": actual_frame.as_str(),
                    "sequence": sequence,
                    "capture_ns": capture.nanoseconds(),
                    "capture_clock_source": capture.clock().source_id().to_hex(),
                    "capture_uncertainty_ns": null,
                    "requested_timestamp_preserved": result.requested_timestamp == capture,
                    "uncertainty_policy": "best_effort_unknown_uncertainty",
                    "transform": {
                        "translation_m": [tx, ty, tz],
                        "rotation_xyzw": [qx, qy, qz, qw]
                    },
                    "sources": sources,
                    "inverse_point_error_m": inverse_point_error_m,
                    "world_lookups": missing_world,
                    "physical_dynamic_tf": "NOT_RUN"
                }));
                if inverse_point_error_m > 1e-9 {
                    bail!("software inverse-point round trip exceeded tolerance");
                }
                "ok"
            }
            Err(error) => {
                emit(json!({
                    "event": "tf_lookup",
                    "status": "error",
                    "error": error.to_string(),
                    "sequence": sequence,
                    "capture_ns": capture.nanoseconds(),
                    "capture_clock_source": capture.clock().source_id().to_hex(),
                    "capture_uncertainty_ns": null,
                    "uncertainty_policy": "best_effort_unknown_uncertainty",
                    "physical_dynamic_tf": "NOT_RUN"
                }));
                "error"
            }
        }
    } else {
        emit(json!({
            "event": "tf_lookup",
            "status": "capture_unknown",
            "sequence": sequence,
            "capture_ns": null,
            "capture_uncertainty_ns": null,
            "physical_dynamic_tf": "NOT_RUN"
        }));
        "capture_unknown"
    };

    emit(json!({
        "event": "tf_camera_frame",
        "sequence": sequence,
        "frame_id": actual_frame.as_str(),
        "width": width,
        "height": height,
        "stride": stride,
        "raw_driver_timestamp_ns": raw_driver_ns,
        "driver_flags": driver_flags,
        "driver_session_generation": driver_session_generation,
        "capture_ns": capture.map(|stamp| stamp.nanoseconds()),
        "capture_clock_source": capture.map(|stamp| stamp.clock().source_id().to_hex()),
        "capture_uncertainty_ns": null,
        "sample_checksum_fnv1a64": checksum,
        "lookup_status": lookup,
        "physical_dynamic_tf": "NOT_RUN"
    }));
    Ok(())
}

fn decode_camera_v2(application: &[u8]) -> anyhow::Result<DecodedCameraV2<'_>> {
    if application.len() < 112 || &application[..8] != b"RBTVCAM2" {
        bail!("invalid or truncated RBTVCAM2 application prefix");
    }
    let width = read_u32(application, 8);
    let height = read_u32(application, 12);
    let stride = read_u32(application, 16);
    let pixel_format = read_u32(application, 20);
    let raw_driver_ns = read_i64(application, 24);
    let driver_flags = read_u32(application, 32);
    let reserved = read_u32(application, 36);
    let driver_session_generation = u64::from_le_bytes(
        application[40..48]
            .try_into()
            .context("read driver session generation")?,
    );
    let frame_bytes: [u8; 64] = application[48..112]
        .try_into()
        .context("read canonical 64-byte frame id")?;
    let frame_id =
        FrameId::from_bytes(frame_bytes).context("validate canonical RBTVCAM2 frame id")?;
    if width == 0
        || height == 0
        || stride < width.saturating_mul(2)
        || pixel_format != V4L2_PIX_FMT_YUYV
        || reserved != 0
    {
        bail!("RBTVCAM2 driver metadata has invalid dimensions, format, or reserved bytes");
    }
    let pixel_len = usize::try_from(u64::from(stride) * u64::from(height))
        .context("YUYV frame size exceeds this process address space")?;
    if pixel_len > MAX_FRAME_BYTES || application.len() != 112 + pixel_len {
        bail!("RBTVCAM2 pixel payload length does not match the bounded stride*height size");
    }
    Ok(DecodedCameraV2 {
        frame_id,
        width,
        height,
        stride,
        raw_driver_ns,
        driver_flags,
        driver_session_generation,
        pixels: &application[112..],
    })
}

fn world_lookup_status(
    buffer: &TransformBuffer,
    capture: Timestamp,
    frame: FrameId,
) -> anyhow::Result<Value> {
    let mut outcomes = serde_json::Map::new();
    for target in ["odom", "map"] {
        let result = buffer.lookup_at_with(
            target,
            frame.as_str(),
            capture,
            LookupOptions {
                timestamp_uncertainty_ns: None,
                mappings: &[],
                now: None,
            },
        );
        match result {
            Ok(_) => bail!("{target} unexpectedly resolved from the static camera graph"),
            Err(
                error @ (rbt_tf::TfError::UnknownFrame
                | rbt_tf::TfError::DisconnectedFrames
                | rbt_tf::TfError::MissingTransform
                | rbt_tf::TfError::InsufficientHistory),
            ) => {
                outcomes.insert(
                    target.to_owned(),
                    json!({
                        "status": "missing_explicitly",
                        "error": error.to_string()
                    }),
                );
            }
            Err(error) => {
                bail!("{target} lookup failed for a reason other than an absent transform: {error}")
            }
        }
    }
    Ok(Value::Object(outcomes))
}

fn host_offset_is_zero(clock: &HostClock) -> bool {
    clock.monotonic_identity().source_id().as_bytes()[32..48]
        .iter()
        .all(|byte| *byte == 0)
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(
        bytes[offset..offset + 4]
            .try_into()
            .expect("validated fixed field"),
    )
}

fn read_i64(bytes: &[u8], offset: usize) -> i64 {
    i64::from_le_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .expect("validated fixed field"),
    )
}

fn fnv1a64(bytes: &[u8]) -> u64 {
    bytes
        .iter()
        .fold(14_695_981_039_346_656_037_u64, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(1_099_511_628_211)
        })
}

fn euclidean_distance(left: Vec3, right: Vec3) -> f64 {
    let (lx, ly, lz) = left.xyz();
    let (rx, ry, rz) = right.xyz();
    (lx - rx).hypot(ly - ry).hypot(lz - rz)
}

fn emit(value: Value) {
    println!("{value}");
    let _ = std::io::stdout().flush();
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[tokio::test]
    async fn calibration_prepare_rejects_oversized_config_before_activation() {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time follows Unix epoch")
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "rbt-tf-camera-oversized-config-{}-{nonce}.toml",
            std::process::id()
        ));
        fs::write(&path, vec![b' '; MAX_STATIC_CONFIG_BYTES + 1])
            .expect("write oversized software fixture");

        let result = <CalibrationComponent as Component>::prepare(&CalibrationConfig {
            component_id: "oversized-config-test".to_owned(),
            path: path.clone(),
        })
        .await;
        let _ = fs::remove_file(path);

        let error = result
            .err()
            .expect("oversized config must fail during prepare");
        assert!(error.to_string().contains("1 MiB input limit"));
    }

    #[test]
    fn camera_jsonl_reader_handles_records_split_across_buffers() {
        let input = b"{\"event\":\"device\"}\n";
        let mut reader = BufReader::with_capacity(3, &input[..]);
        let CameraJsonlRecord::Complete(record) =
            read_camera_jsonl_record(&mut reader).expect("read complete record")
        else {
            panic!("newline-terminated JSONL record should be complete");
        };
        assert_eq!(record, input);
    }

    #[test]
    fn camera_jsonl_reader_rejects_incomplete_records() {
        let input = b"{\"event\":\"device\"}";
        let mut reader = BufReader::with_capacity(3, &input[..]);
        assert!(matches!(
            read_camera_jsonl_record(&mut reader).expect("read incomplete record"),
            CameraJsonlRecord::Incomplete
        ));
    }

    #[test]
    fn camera_jsonl_reader_discards_oversized_records_without_growing() {
        let mut input = vec![b'x'; MAX_CAMERA_CHILD_JSONL_RECORD_BYTES + 32];
        input.extend_from_slice(b"\n{\"event\":\"device\"}\n");
        let mut reader = BufReader::with_capacity(7, &input[..]);
        assert!(matches!(
            read_camera_jsonl_record(&mut reader).expect("read oversized record"),
            CameraJsonlRecord::Oversized
        ));
        let CameraJsonlRecord::Complete(record) =
            read_camera_jsonl_record(&mut reader).expect("read record after oversized input")
        else {
            panic!("reader should resume after discarding the oversized record");
        };
        assert_eq!(record, b"{\"event\":\"device\"}\n");
    }

    #[test]
    fn rbtvcam2_identity_and_payload_are_canonical_and_bounded() {
        let mut app = vec![0_u8; 112 + 8];
        app[..8].copy_from_slice(b"RBTVCAM2");
        app[8..12].copy_from_slice(&2_u32.to_le_bytes());
        app[12..16].copy_from_slice(&2_u32.to_le_bytes());
        app[16..20].copy_from_slice(&4_u32.to_le_bytes());
        app[20..24].copy_from_slice(&V4L2_PIX_FMT_YUYV.to_le_bytes());
        let frame = FrameId::new("camera_optical").expect("valid fixture frame");
        app[48..112].copy_from_slice(&frame.to_bytes());
        app[112..].copy_from_slice(b"pixels!!");
        let decoded = decode_camera_v2(&app).unwrap();
        assert_eq!(decoded.frame_id, frame);
        assert_eq!((decoded.width, decoded.height, decoded.stride), (2, 2, 4));
        assert_eq!(decoded.driver_session_generation, 0);
        assert_eq!(decoded.pixels, b"pixels!!");
        app[111] = 0x20;
        assert!(decode_camera_v2(&app).is_err());
    }

    #[test]
    fn rbtvcam2_requires_v2_magic_and_exact_pixels() {
        let mut app = vec![0_u8; 112];
        app[..8].copy_from_slice(b"RBTVCAM1");
        assert!(decode_camera_v2(&app).is_err());
    }
}

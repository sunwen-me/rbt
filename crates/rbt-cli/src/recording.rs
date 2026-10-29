use std::{
    collections::{BTreeMap, HashSet},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use rbt_compose::{
    CONTROL_PROTOCOL_VERSION, CapabilityInspection, CapabilityTransportInspection, InspectionView,
};
use rbt_iox2::{
    ActivationId, ControlCommand, Image, PointCloud, ProcessInstanceId, ReadinessProtocol,
    SliceStream, Stream, StreamDescriptor, StreamProfile, WaitContext, WaitProgress, WaitToken,
};
use rbt_mcap::{
    ByteQueue, ByteQueueConsumer, ChannelDefinition, ChannelProvenance, KnownMessageType,
    QueueError, QueueItem, RecordingMetadata, RecordingWriter, encode_control, encode_image,
    encode_pointcloud,
};
use rbt_types::{MESSAGE_DESCRIPTOR_VERSION, MessageTypeDescriptor, TransportFeatures};
use serde::Serialize;

use crate::CliError;

/// Configuration for one fixed-set recording session.
#[derive(Clone, Debug)]
pub struct RecordOptions {
    pub output: std::path::PathBuf,
    pub capabilities: Vec<String>,
    pub all: bool,
    pub buffer_bytes: usize,
    pub duration: Option<Duration>,
}

/// Per-channel recorder-side completeness counters.
#[derive(Clone, Debug, Default, Serialize)]
pub struct ChannelRecordSummary {
    pub topic: String,
    pub messages_recorded: u64,
    pub payload_bytes: u64,
    pub transport_sequence_gaps: u64,
    pub recorder_drops: u64,
    pub first_log_time_ns: Option<u64>,
    pub last_log_time_ns: Option<u64>,
}

/// Final recording summary written into the file and printed by the CLI.
#[derive(Clone, Debug, Serialize)]
pub struct RecordSummary {
    pub output: std::path::PathBuf,
    pub complete: bool,
    pub channels: Vec<ChannelRecordSummary>,
    pub messages_recorded: u64,
    pub payload_bytes: u64,
    pub transport_sequence_gaps: u64,
    pub recorder_drops: u64,
    pub writer_errors: u64,
    pub first_log_time_ns: Option<u64>,
    pub last_log_time_ns: Option<u64>,
    pub duration_ns: u64,
    pub queue_high_water_bytes: usize,
    pub queue_budget_bytes: usize,
    pub file_size_bytes: u64,
}

enum CaptureSource {
    Image(rbt_iox2::SliceSubscriber<Image>),
    PointCloud(rbt_iox2::SliceSubscriber<PointCloud>),
    Control(rbt_iox2::Subscriber<ControlCommand>),
}

impl CaptureSource {
    fn open(capability: &CapabilityInspection) -> Result<Self, CliError> {
        let (message_type, profile, readiness) = recording_stream(capability)?;
        let kind = KnownMessageType::from_descriptor(message_type)?;
        let descriptor = StreamDescriptor::new_with_transport(
            capability.physical_endpoint.clone(),
            message_type.clone(),
            profile,
            TransportFeatures { readiness },
        );
        match kind {
            KnownMessageType::Image => {
                Ok(Self::Image(SliceStream::<Image>::subscriber(&descriptor)?))
            }
            KnownMessageType::PointCloud => Ok(Self::PointCloud(
                SliceStream::<PointCloud>::subscriber(&descriptor)?,
            )),
            KnownMessageType::ControlCommand => Ok(Self::Control(
                Stream::<ControlCommand>::subscriber(&descriptor)?,
            )),
        }
    }

    fn event_source(&self, token: WaitToken) -> rbt_iox2::EventSource<'_> {
        match self {
            Self::Image(subscriber) => subscriber.event_source(token),
            Self::PointCloud(subscriber) => subscriber.event_source(token),
            Self::Control(subscriber) => subscriber.event_source(token),
        }
    }

    fn drain(
        &self,
        budget: usize,
        channel_id: u16,
        profile: StreamProfile,
        clock: &RecordingClock,
        queue: &ByteQueue,
        stats: &mut ChannelStats,
    ) -> Result<WaitProgress, CliError> {
        let mut samples = 0;
        let mut observed_empty = false;
        for _ in 0..budget {
            let item = match self {
                Self::Image(subscriber) => subscriber.receive()?.map(|sample| {
                    let sequence = sample.header().sequence;
                    let log_time = clock.now();
                    let data = encode_image(sample.header(), sample.payload());
                    (sequence, log_time, data)
                }),
                Self::PointCloud(subscriber) => subscriber.receive()?.map(|sample| {
                    let sequence = sample.header().sequence;
                    let log_time = clock.now();
                    let data = encode_pointcloud(sample.header(), sample.payload());
                    (sequence, log_time, data)
                }),
                Self::Control(subscriber) => subscriber.receive()?.map(|sample| {
                    let sequence = sample.sequence;
                    let log_time = clock.now();
                    let data = encode_control(&sample);
                    (sequence, log_time, data)
                }),
            };
            let Some((sequence, log_time, data)) = item else {
                observed_empty = true;
                break;
            };
            samples += 1;

            stats.observe_sequence(sequence);
            let data_len = data.len();
            match queue.try_push(QueueItem {
                channel_id,
                sequence,
                log_time,
                profile,
                data,
            }) {
                Ok(()) => {
                    stats.messages_recorded = stats.messages_recorded.saturating_add(1);
                    stats.payload_bytes = stats.payload_bytes.saturating_add(data_len as u64);
                    stats.first_log_time.get_or_insert(log_time);
                    stats.last_log_time = Some(log_time);
                }
                Err(QueueError::Full { .. }) if profile == StreamProfile::Sensor => {
                    stats.recorder_drops = stats.recorder_drops.saturating_add(1);
                }
                Err(QueueError::Full { .. }) => {
                    return Err(CliError::Recording(format!(
                        "Control channel {} exceeded the {}-byte writer budget; the command was not silently dropped",
                        stats.topic,
                        queue.byte_usage()?.2
                    )));
                }
                Err(error) => return Err(error.into()),
            }
        }
        Ok(match (samples, observed_empty) {
            (0, true) => WaitProgress::Empty,
            (_, true) => WaitProgress::SamplesAndEmpty {
                samples,
                empty_wakeups: 1,
            },
            _ => WaitProgress::Samples(samples),
        })
    }
}

#[derive(Debug)]
struct ChannelStats {
    topic: String,
    messages_recorded: u64,
    payload_bytes: u64,
    transport_sequence_gaps: u64,
    recorder_drops: u64,
    previous_sequence: Option<u64>,
    first_log_time: Option<u64>,
    last_log_time: Option<u64>,
}

impl ChannelStats {
    fn new(topic: String) -> Self {
        Self {
            topic,
            messages_recorded: 0,
            payload_bytes: 0,
            transport_sequence_gaps: 0,
            recorder_drops: 0,
            previous_sequence: None,
            first_log_time: None,
            last_log_time: None,
        }
    }

    fn observe_sequence(&mut self, sequence: u64) {
        if let Some(previous) = self.previous_sequence {
            self.transport_sequence_gaps = self
                .transport_sequence_gaps
                .saturating_add(sequence.saturating_sub(previous).saturating_sub(1));
        }
        self.previous_sequence = Some(sequence);
    }

    fn snapshot(&self) -> ChannelRecordSummary {
        ChannelRecordSummary {
            topic: self.topic.clone(),
            messages_recorded: self.messages_recorded,
            payload_bytes: self.payload_bytes,
            transport_sequence_gaps: self.transport_sequence_gaps,
            recorder_drops: self.recorder_drops,
            first_log_time_ns: self.first_log_time,
            last_log_time_ns: self.last_log_time,
        }
    }
}

struct RecordingClock {
    epoch_ns: u64,
    monotonic_start: Instant,
}

struct CaptureSession {
    output: std::path::PathBuf,
    sources: Vec<CaptureSource>,
    definitions: Vec<ChannelDefinition>,
    channel_ids: Vec<u16>,
    writer: RecordingWriter,
    epoch_ns: u64,
    buffer_bytes: usize,
}

impl RecordingClock {
    fn now(&self) -> u64 {
        let elapsed = u64::try_from(self.monotonic_start.elapsed().as_nanos()).unwrap_or(u64::MAX);
        self.epoch_ns.saturating_add(elapsed)
    }
}

/// Resolves a fixed initial topology, records until duration/signal, and finalizes MCAP.
pub async fn run_record(
    view: &InspectionView,
    options: RecordOptions,
) -> Result<RecordSummary, CliError> {
    let selected = resolve_capabilities(view, &options.capabilities, options.all)?;
    eprintln!(
        "selected {} fixed physical recording source(s):",
        selected.len()
    );
    for capability in &selected {
        let (message_type, _, _) = recording_stream(capability)?;
        eprintln!(
            "  {} {} provider={} capability={} endpoint={}",
            capability.capability_type,
            message_type.qualified_name(),
            capability.provider_component,
            capability.capability_instance,
            capability.physical_endpoint,
        );
    }
    let definitions = selected
        .iter()
        .map(|capability| channel_definition(capability))
        .collect::<Result<Vec<_>, _>>()?;

    // Open every fixed physical source before recording starts. The endpoint
    // names include physical capability identity, so provider replacement is
    // never followed silently during this v0.3 session.
    let sources = selected
        .iter()
        .map(|capability| CaptureSource::open(capability))
        .collect::<Result<Vec<_>, _>>()?;

    let epoch_ns = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| CliError::Recording(format!("system clock precedes Unix epoch: {error}")))?
        .as_nanos()
        .try_into()
        .unwrap_or(u64::MAX);
    let metadata = RecordingMetadata {
        rbt_sdk_version: env!("CARGO_PKG_VERSION").to_owned(),
        idl_language_version: 1,
        message_descriptor_version: MESSAGE_DESCRIPTOR_VERSION,
        control_protocol_version: CONTROL_PROTOCOL_VERSION,
        host_architecture: std::env::consts::ARCH.to_owned(),
        endianness: "little".to_owned(),
        recording_start_time_ns: epoch_ns,
    };
    let writer = RecordingWriter::create(&options.output, &definitions, &metadata)?;
    let channel_ids = (0..definitions.len())
        .map(|index| {
            writer
                .channel_id(index)
                .ok_or_else(|| CliError::Recording(format!("writer omitted channel index {index}")))
        })
        .collect::<Result<Vec<_>, _>>()?;

    let mut wait = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())?;
    let wake = wait.wake_handle();
    let output = options.output.clone();
    let buffer_bytes = options.buffer_bytes;
    let mut worker = tokio::task::spawn_blocking(move || {
        capture_session(
            CaptureSession {
                output,
                sources,
                definitions,
                channel_ids,
                writer,
                epoch_ns,
                buffer_bytes,
            },
            &mut wait,
        )
    });

    let stop_reason = async {
        match options.duration {
            Some(duration) => {
                tokio::select! {
                    signal = shutdown_signal() => signal.map(|()| "signal"),
                    _ = tokio::time::sleep(duration) => Ok("duration"),
                }
            }
            None => shutdown_signal().await.map(|()| "signal"),
        }
    };
    tokio::pin!(stop_reason);

    tokio::select! {
        result = &mut worker => result.map_err(|error| CliError::Join(error.to_string()))?,
        stop = &mut stop_reason => {
            let _ = stop.map_err(|error| CliError::Recording(format!("failed to install shutdown signal handler: {error}")))?;
            wake.wake()?;
            worker.await.map_err(|error| CliError::Join(error.to_string()))?
        }
    }
}

fn capture_session(
    session: CaptureSession,
    wait: &mut WaitContext,
) -> Result<RecordSummary, CliError> {
    let CaptureSession {
        output,
        sources,
        definitions,
        channel_ids,
        writer,
        epoch_ns,
        buffer_bytes,
    } = session;
    let (queue, consumer) = ByteQueue::new(buffer_bytes);
    let writer_wake = wait.wake_handle();
    let writer_thread = thread::Builder::new()
        .name("rbt-mcap-writer".to_owned())
        .spawn(move || writer_loop(writer, consumer, writer_wake))
        .map_err(|error| CliError::Recording(format!("failed to spawn writer thread: {error}")))?;

    let clock = RecordingClock {
        epoch_ns,
        monotonic_start: Instant::now(),
    };
    let event_sources = sources
        .iter()
        .enumerate()
        .map(|(index, source)| source.event_source(WaitToken::new(index as u64 + 1)))
        .collect::<Vec<_>>();
    let mut stats = definitions
        .iter()
        .map(|definition| ChannelStats::new(definition.topic.clone()))
        .collect::<Vec<_>>();
    let mut last_progress = Instant::now();
    let capture_result: Result<_, CliError> = wait.run(&event_sources, |event, budget| {
        let index = usize::try_from(event.token().value().saturating_sub(1)).unwrap_or(usize::MAX);
        let source = sources.get(index).ok_or_else(|| {
            CliError::Recording(format!("unknown readiness token {}", event.token()))
        })?;
        let progress = source.drain(
            budget,
            channel_ids[index],
            definitions[index].profile,
            &clock,
            &queue,
            &mut stats[index],
        )?;
        if last_progress.elapsed() >= Duration::from_secs(1) {
            let messages = stats.iter().map(|value| value.messages_recorded).sum::<u64>();
            let drops = stats.iter().map(|value| value.recorder_drops).sum::<u64>();
            let (used, high_water, budget) = queue.byte_usage()?;
            eprintln!(
                "record progress messages={messages} drops={drops} queue={used}/{budget} high_water={high_water}"
            );
            last_progress = Instant::now();
        }
        Ok(progress)
    });
    queue.close()?;
    let writer = writer_thread
        .join()
        .map_err(|_| CliError::Join("MCAP writer thread panicked".to_owned()))??;
    let (_, high_water, budget) = queue.byte_usage()?;

    let channels = stats.iter().map(ChannelStats::snapshot).collect::<Vec<_>>();
    let messages_recorded = channels.iter().map(|value| value.messages_recorded).sum();
    let payload_bytes = channels.iter().map(|value| value.payload_bytes).sum();
    let transport_sequence_gaps = channels
        .iter()
        .map(|value| value.transport_sequence_gaps)
        .sum();
    let recorder_drops = channels.iter().map(|value| value.recorder_drops).sum();
    let first_log_time_ns = channels
        .iter()
        .filter_map(|value| value.first_log_time_ns)
        .min();
    let last_log_time_ns = channels
        .iter()
        .filter_map(|value| value.last_log_time_ns)
        .max();
    let mut summary = RecordSummary {
        output,
        complete: capture_result.is_ok() && recorder_drops == 0,
        channels,
        messages_recorded,
        payload_bytes,
        transport_sequence_gaps,
        recorder_drops,
        writer_errors: 0,
        first_log_time_ns,
        last_log_time_ns,
        duration_ns: last_log_time_ns
            .zip(first_log_time_ns)
            .map_or(0, |(last, first)| last.saturating_sub(first)),
        queue_high_water_bytes: high_water,
        queue_budget_bytes: budget,
        file_size_bytes: 0,
    };
    writer.finish(summary_metadata(&summary))?;
    summary.file_size_bytes = std::fs::metadata(&summary.output)
        .map_err(|error| {
            CliError::Recording(format!(
                "failed to stat finalized recording {}: {error}",
                summary.output.display()
            ))
        })?
        .len();
    capture_result?;
    Ok(summary)
}

fn writer_loop(
    mut writer: RecordingWriter,
    consumer: ByteQueueConsumer,
    wake: rbt_iox2::WaitWakeHandle,
) -> Result<RecordingWriter, CliError> {
    loop {
        let Some(item) = consumer.pop()? else {
            return Ok(writer);
        };
        if let Err(error) =
            writer.write_message(item.channel_id, item.sequence, item.log_time, &item.data)
        {
            let _ = consumer.fail(error.to_string());
            let _ = wake.wake();
            return Err(error.into());
        }
    }
}

fn resolve_capabilities<'a>(
    view: &'a InspectionView,
    selectors: &[String],
    all: bool,
) -> Result<Vec<&'a CapabilityInspection>, CliError> {
    let mut selected = Vec::new();
    if all {
        selected.extend(view.capability_topology.iter().filter(|capability| {
            let CapabilityTransportInspection::Stream { message_type, .. } = &capability.transport
            else {
                return false;
            };
            KnownMessageType::from_descriptor(message_type).is_ok()
        }));
        selected.sort_by_key(|capability| {
            (
                capability.capability_type.clone(),
                capability.provider_component.to_string(),
                capability.capability_instance.to_string(),
            )
        });
    } else {
        for selector in selectors {
            let matches = view
                .capability_topology
                .iter()
                .filter(|capability| selector_matches(capability, selector))
                .collect::<Vec<_>>();
            match matches.as_slice() {
                [] => {
                    return Err(CliError::NotFound(format!(
                        "recording capability selector {selector:?} matched no active capability"
                    )));
                }
                [capability] => selected.push(*capability),
                _ => {
                    return Err(CliError::Usage(format!(
                        "recording capability selector {selector:?} is ambiguous; use its capability UUID or physical endpoint"
                    )));
                }
            }
        }
    }
    let mut identities = HashSet::new();
    selected.retain(|capability| identities.insert(capability.capability_instance));
    if selected.is_empty() {
        return Err(CliError::NotFound(
            "no supported Image, PointCloud, or ControlCommand capabilities are active".to_owned(),
        ));
    }
    for capability in &selected {
        let (message_type, _, readiness) = recording_stream(capability)?;
        KnownMessageType::from_descriptor(message_type)?;
        if readiness != ReadinessProtocol::SidecarV1 {
            return Err(CliError::Recording(format!(
                "capability {} does not provide sidecar-v1 readiness",
                capability.capability_instance
            )));
        }
    }
    Ok(selected)
}

fn selector_matches(capability: &CapabilityInspection, selector: &str) -> bool {
    capability.capability_type.eq_ignore_ascii_case(selector)
        || capability.provider_component.as_ref() == selector
        || capability.physical_endpoint == selector
        || capability.capability_instance.to_string() == selector
}

fn channel_definition(capability: &CapabilityInspection) -> Result<ChannelDefinition, CliError> {
    let (message_type, profile, _) = recording_stream(capability)?;
    let provider = slug(capability.provider_component.as_ref());
    let capability_name = slug(&capability.capability_type);
    Ok(ChannelDefinition {
        topic: format!("/{provider}/{capability_name}"),
        message_type: message_type.clone(),
        profile,
        capability_type: capability.capability_type.clone(),
        capability_name,
        capability_qualifier: String::new(),
        provenance: ChannelProvenance {
            source_component: capability.provider_component.to_string(),
            source_process_instance: capability.provider_process.to_string(),
            source_activation: capability.activation.value().to_string(),
            source_capability_instance: capability.capability_instance.to_string(),
        },
    })
}

fn recording_stream(
    capability: &CapabilityInspection,
) -> Result<(&MessageTypeDescriptor, StreamProfile, ReadinessProtocol), CliError> {
    match &capability.transport {
        CapabilityTransportInspection::Stream {
            message_type,
            profile,
            readiness,
            ..
        } => Ok((message_type, *profile, *readiness)),
        CapabilityTransportInspection::Rpc { .. } => Err(CliError::Recording(format!(
            "capability {} uses RPC; MCAP recording remains Stream-only",
            capability.capability_instance
        ))),
    }
}

fn slug(value: &str) -> String {
    let mut result = String::new();
    for character in value.chars() {
        if character.is_ascii_alphanumeric() {
            result.push(character.to_ascii_lowercase());
        } else if !result.ends_with('-') {
            result.push('-');
        }
    }
    result.trim_matches('-').to_owned()
}

fn summary_metadata(summary: &RecordSummary) -> BTreeMap<String, String> {
    let mut metadata = BTreeMap::from([
        ("complete".to_owned(), summary.complete.to_string()),
        (
            "messages_recorded".to_owned(),
            summary.messages_recorded.to_string(),
        ),
        (
            "payload_bytes".to_owned(),
            summary.payload_bytes.to_string(),
        ),
        (
            "transport_sequence_gaps".to_owned(),
            summary.transport_sequence_gaps.to_string(),
        ),
        (
            "recorder_drops".to_owned(),
            summary.recorder_drops.to_string(),
        ),
        (
            "writer_errors".to_owned(),
            summary.writer_errors.to_string(),
        ),
        (
            "queue_high_water_bytes".to_owned(),
            summary.queue_high_water_bytes.to_string(),
        ),
        (
            "queue_budget_bytes".to_owned(),
            summary.queue_budget_bytes.to_string(),
        ),
    ]);
    for (index, channel) in summary.channels.iter().enumerate() {
        metadata.insert(format!("channel.{index}.topic"), channel.topic.clone());
        metadata.insert(
            format!("channel.{index}.messages_recorded"),
            channel.messages_recorded.to_string(),
        );
        metadata.insert(
            format!("channel.{index}.recorder_drops"),
            channel.recorder_drops.to_string(),
        );
        metadata.insert(
            format!("channel.{index}.transport_sequence_gaps"),
            channel.transport_sequence_gaps.to_string(),
        );
    }
    metadata
}

#[cfg(unix)]
async fn shutdown_signal() -> std::io::Result<()> {
    let mut sigint = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt())?;
    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
    tokio::select! {
        _ = sigint.recv() => Ok(()),
        _ = sigterm.recv() => Ok(()),
    }
}

#[cfg(not(unix))]
async fn shutdown_signal() -> std::io::Result<()> {
    tokio::signal::ctrl_c().await
}

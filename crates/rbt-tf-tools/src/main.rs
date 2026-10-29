use std::{
    collections::BTreeMap,
    env, fs,
    io::Write as _,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    thread,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, anyhow, bail};
use async_trait::async_trait;
use rbt::{
    component::{Component, run_component},
    types::{Capability, CapabilityInstanceId, ComponentId, RequirementSpec},
};
use rbt_compose::ActivationCtx;
use rbt_core::ActivationId;
use rbt_iox2::{SliceStream, Stream, StreamProfile, WaitContext, WaitProgress, WaitToken};
use rbt_tf::{
    DynamicRecord, LookupOptions, LookupPolicy, Quaternion, StaticEdge, StaticSet, TfError,
    TfResourceConfig, Transform3, TransformBuffer, Vec3,
};
use rbt_tf_transport::{
    DynamicTfPublisher, DynamicTfSubscriber, StaticTfClient, StaticTfProvider,
    TransformActivationExt,
};
use rbt_time::{
    ClockMapping, ClockMappingCalibration, ClockSourceId, DeviceClock, HostClock, TimeError,
    Timestamp,
};
use rbt_types::{
    DynamicTransform, RpcDescriptor, RpcResourceConfig, StaticSnapshot, StaticSnapshotRequest,
    StaticUpdate,
};
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

const FIXTURE_NOTICE: &str = "synthetic software fixture; not physical measurement";
const DEFAULT_COMPOSER: &str = "127.0.0.1:7447";
const MAX_STATIC_TRANSFORMS: usize = 64;

struct StaticUpdateRole;
struct StaticSnapshotRole;
struct DynamicRole;

impl Capability for StaticUpdateRole {
    const TYPE_NAME: &'static str = "TfStaticUpdate";
}

impl Capability for StaticSnapshotRole {
    const TYPE_NAME: &'static str = "TfStaticSnapshot";
}

impl Capability for DynamicRole {
    const TYPE_NAME: &'static str = "TfDynamic";
}

fn main() {
    if let Err(error) = dispatch() {
        eprintln!("RBT_TF_TOOLS ERROR {error:#}");
        std::process::exit(1);
    }
}

fn dispatch() -> Result<()> {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        Some("semantics") => semantics(),
        Some("bench") => benchmark(parse_options(args.collect())?),
        Some("composer") => run_composer(parse_options(args.collect())?),
        Some("provider") => run_provider(parse_options(args.collect())?),
        Some("consumer") => run_consumer(parse_options(args.collect())?),
        Some("interop") => {
            let direction = args
                .next()
                .ok_or_else(|| anyhow!("interop requires publish or consume"))?;
            let options = parse_options(args.collect())?;
            match direction.as_str() {
                "publish" => interop_publish(options),
                "consume" => interop_consume(options),
                _ => bail!("interop mode must be publish or consume"),
            }
        }
        Some("--help" | "-h") | None => {
            print_usage();
            Ok(())
        }
        Some(command) => bail!("unknown command {command:?}; use --help"),
    }
}

fn print_usage() {
    eprintln!(
        "usage: rbt-tf-tools semantics | bench [--queries N] | composer [--addr ADDR] | provider|consumer [--addr ADDR] | interop publish|consume --static-id UUID --dynamic-id UUID --snapshot-id UUID --ready PATH --peer-ready PATH --done PATH --timeout-sec N"
    );
}

fn parse_options(args: Vec<String>) -> Result<BTreeMap<String, String>> {
    let mut values = BTreeMap::new();
    let mut index = 0;
    while index < args.len() {
        let name = &args[index];
        if !name.starts_with("--") {
            bail!("unexpected positional argument {name:?}");
        }
        let value = args
            .get(index + 1)
            .ok_or_else(|| anyhow!("missing value for {name}"))?;
        if value.starts_with("--") {
            bail!("missing value for {name}");
        }
        if values.insert(name.clone(), value.clone()).is_some() {
            bail!("duplicate option {name}");
        }
        index += 2;
    }
    Ok(values)
}

fn option<'a>(values: &'a BTreeMap<String, String>, name: &str, default: &'a str) -> &'a str {
    values.get(name).map(String::as_str).unwrap_or(default)
}

fn only_options(values: &BTreeMap<String, String>, allowed: &[&str]) -> Result<()> {
    for name in values.keys() {
        if !allowed.contains(&name.as_str()) {
            bail!("unexpected option {name}");
        }
    }
    Ok(())
}

fn semantics() -> Result<()> {
    let fixture_clock = DeviceClock::new([0x11; 16], 7)?;
    let other_clock = DeviceClock::new([0x22; 16], 4)?;
    let host = HostClock::new()?;
    let now = host.monotonic_now()?;
    let buffer = TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default())?;
    let authority = buffer.bind_provider("semantics_fixture", CapabilityInstanceId::new())?;
    buffer.apply_static_snapshot(
        &authority,
        StaticSet {
            revision: 10,
            effective_from: fixture_clock.stamp(0),
            effective_uncertainty_ns: Some(0),
            transforms: vec![
                StaticEdge {
                    parent: rbt_tf::FrameId::new("map")?,
                    child: rbt_tf::FrameId::new("odom")?,
                    transform: translated(10.0, 0.0, 0.0)?,
                },
                StaticEdge {
                    parent: rbt_tf::FrameId::new("base")?,
                    child: rbt_tf::FrameId::new("camera")?,
                    transform: translated(1.0, 0.0, 0.0)?,
                },
            ],
        },
    )?;
    for (ns, x, rotation) in [
        (100_000_000, 0.0, Quaternion::identity()),
        (
            200_000_000,
            2.0,
            Quaternion::new(
                0.0,
                0.0,
                std::f64::consts::FRAC_1_SQRT_2,
                std::f64::consts::FRAC_1_SQRT_2,
            )?,
        ),
    ] {
        buffer.ingest_dynamic(
            &authority,
            DynamicRecord {
                parent: rbt_tf::FrameId::new("odom")?,
                child: rbt_tf::FrameId::new("base")?,
                stamp: fixture_clock.stamp(ns),
                uncertainty_ns: Some(0),
                transform_epoch: 3,
                transform: Transform3::new(Vec3::new(x, 0.0, 0.0)?, rotation),
            },
        )?;
    }
    let query = fixture_clock.stamp(150_000_000);
    let direction = buffer.lookup_at_with("base", "camera", query, known_query_options())?;
    let direction_point = direction.apply_point(Vec3::new(0.0, 0.0, 1.0)?)?;
    let inverse_point = direction
        .transform
        .inverse()?
        .apply_point(direction_point)?;
    let interpolated = buffer.lookup_at_with("odom", "base", query, known_query_options())?;
    let mapped = buffer.lookup_at_with("odom", "camera", query, known_query_options())?;
    let mapped_point = mapped.apply_point(Vec3::zero())?;
    let chain = buffer.lookup_at_with("map", "camera", query, known_query_options())?;
    let chain_point = chain.apply_point(Vec3::zero())?;

    let valid_map = make_clock_mapping(
        other_clock.identity(),
        fixture_clock.identity(),
        0x53,
        150_000_000,
        150_000_000,
        now,
        now.checked_add_nanoseconds(5_000_000_000)?,
    )?;
    let valid_mappings = [&valid_map];
    let valid_mapped = buffer.lookup_at_with(
        "odom",
        "camera",
        other_clock.stamp(150_000_000),
        LookupOptions {
            timestamp_uncertainty_ns: Some(0),
            mappings: &valid_mappings,
            now: Some(&now),
        },
    )?;
    let valid_mapped_point = valid_mapped.apply_point(Vec3::zero())?;
    let wrong_clock_error = match buffer.lookup_at_with(
        "odom",
        "camera",
        other_clock.stamp(150_000_000),
        known_query_options(),
    ) {
        Err(TfError::Time(TimeError::ClockMappingUnavailable)) => "clock_mapping_unavailable",
        Err(error) => bail!("wrong-clock fixture returned {error:?}"),
        Ok(_) => bail!("wrong-clock fixture unexpectedly succeeded"),
    };
    let expired_map = make_clock_mapping(
        other_clock.identity(),
        fixture_clock.identity(),
        0x54,
        150_000_000,
        150_000_000,
        now.checked_add_nanoseconds(-1_000_000)?,
        now,
    )?;
    let expired_mappings = [&expired_map];
    let expired_mapping_error = match buffer.lookup_at_with(
        "odom",
        "camera",
        other_clock.stamp(150_000_000),
        LookupOptions {
            timestamp_uncertainty_ns: Some(0),
            mappings: &expired_mappings,
            now: Some(&now),
        },
    ) {
        Err(TfError::Time(TimeError::ClockMappingExpired)) => "clock_mapping_expired",
        Err(error) => bail!("expired mapping fixture returned {error:?}"),
        Ok(_) => bail!("expired mapping fixture unexpectedly succeeded"),
    };
    let root2 = std::f64::consts::FRAC_1_SQRT_2;
    if (direction_point.x() - 1.0).abs() > 1.0e-9
        || (direction_point.z() - 1.0).abs() > 1.0e-9
        || inverse_point.x().abs() > 1.0e-9
        || (inverse_point.z() - 1.0).abs() > 1.0e-9
        || (chain_point.x() - (11.0 + root2)).abs() > 1.0e-9
        || (chain_point.y() - root2).abs() > 1.0e-9
        || (interpolated.transform.translation().x() - 1.0).abs() > 1.0e-9
        || (mapped_point.x() - (1.0 + root2)).abs() > 1.0e-9
        || (valid_mapped_point.x() - mapped_point.x()).abs() > 1.0e-9
    {
        bail!(
            "TF semantics fixture returned an incorrect direction, chain, or interpolation result"
        );
    }

    let output = json!({
        "diagnostics": FIXTURE_NOTICE,
        "language": "rust",
        "direction_point": vector(direction_point),
        "inverse_point": vector(inverse_point),
        "chain_point": vector(chain_point),
        "interpolated_translation": vector(interpolated.transform.translation()),
        "interpolated_quaternion_xyzw": quaternion(interpolated.transform.rotation()),
        "wrong_clock_error": wrong_clock_error,
        "expired_mapping_error": expired_mapping_error,
        "mapped_point": vector(valid_mapped_point),
    });
    println!("{}", serde_json::to_string(&output)?);
    Ok(())
}

fn known_query_options() -> LookupOptions<'static> {
    LookupOptions {
        timestamp_uncertainty_ns: Some(0),
        mappings: &[],
        now: None,
    }
}

#[allow(clippy::too_many_arguments)]
fn make_clock_mapping(
    source: rbt_time::ClockIdentity,
    target: rbt_time::ClockIdentity,
    calibration_byte: u8,
    source_anchor_ns: i64,
    target_anchor_ns: i64,
    calibrated_at: Timestamp,
    expires_at: Timestamp,
) -> Result<ClockMapping> {
    Ok(ClockMapping::new(ClockMappingCalibration {
        source,
        target,
        source_anchor_ns,
        target_anchor_ns,
        rate_numerator: 1,
        rate_denominator: 1,
        valid_from_ns: 0,
        valid_until_ns: 300_000_000,
        calibration_id: [calibration_byte; 16],
        calibrated_at,
        expires_at,
        uncertainty_ns: Some(0),
    })?)
}

fn benchmark(options: BTreeMap<String, String>) -> Result<()> {
    only_options(&options, &["--queries"])?;
    let queries = option(&options, "--queries", "10000")
        .parse::<usize>()
        .context("--queries must be a positive integer")?;
    if queries == 0 || queries > 10_000_000 {
        bail!("--queries must be in 1..=10000000");
    }
    let clock = HostClock::new()?;
    let stamp = clock.monotonic_now()?;
    let mut cases = Vec::new();
    for edges in [1_usize, 3, 10] {
        let buffer = static_chain_buffer(edges, stamp)?;
        let before = rss_kib().unwrap_or(0);
        let samples = timed_queries(queries, || {
            let result = buffer.lookup_at_with(
                "bench_0",
                &format!("bench_{edges}"),
                stamp,
                LookupOptions {
                    timestamp_uncertainty_ns: Some(0),
                    mappings: &[],
                    now: None,
                },
            )?;
            let expected = edges as f64;
            if (result.transform.translation().x() - expected).abs() > 1.0e-9 {
                bail!("{edges}-edge benchmark returned an incorrect transform");
            }
            Ok(())
        })?;
        let after = rss_kib().unwrap_or(before);
        cases.push(latency_json(
            format!("static_{edges}_edge"),
            queries,
            samples,
            before,
            after,
            "local_lookup",
        ));
    }

    let (history, query_stamp) = history_buffer(&clock)?;
    let before = rss_kib().unwrap_or(0);
    let samples = timed_queries(queries, || {
        let result = history.lookup_at_with(
            "history_parent",
            "history_child",
            query_stamp,
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                mappings: &[],
                now: None,
            },
        )?;
        if (result.transform.translation().x() - 5.0).abs() > 1.0e-6 {
            bail!(
                "history interpolation benchmark returned x={}, expected 5.0 at {} ns",
                result.transform.translation().x(),
                query_stamp.nanoseconds()
            );
        }
        Ok(())
    })?;
    let after = rss_kib().unwrap_or(before);
    cases.push(latency_json(
        "history_interpolation".to_owned(),
        queries,
        samples,
        before,
        after,
        "local_lookup",
    ));

    println!(
        "{}",
        serde_json::to_string(&json!({
            "fixture": FIXTURE_NOTICE,
            "measurement_scope": "local TransformBuffer lookup only; transport startup, delivery, and RPC are excluded",
            "queries_per_case": queries,
            "transport_included": false,
            "cases": cases,
        }))?
    );
    Ok(())
}

fn static_chain_buffer(edges: usize, stamp: Timestamp) -> Result<TransformBuffer> {
    let resources = TfResourceConfig {
        max_total_edges: edges.max(1),
        max_static_transforms_per_provider: edges.max(1),
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default())?;
    let authority = buffer.bind_provider("bench_provider", CapabilityInstanceId::new())?;
    let mut transforms = Vec::with_capacity(edges);
    for index in 0..edges {
        let parent = format!("bench_{index}");
        let child = format!("bench_{}", index + 1);
        transforms.push(StaticEdge {
            parent: rbt_tf::FrameId::new(&parent)?,
            child: rbt_tf::FrameId::new(&child)?,
            transform: translated(1.0, 0.0, 0.0)?,
        });
    }
    buffer.apply_static_snapshot(
        &authority,
        StaticSet {
            revision: 1,
            effective_from: stamp,
            effective_uncertainty_ns: Some(0),
            transforms,
        },
    )?;
    Ok(buffer)
}

fn history_buffer(clock: &HostClock) -> Result<(TransformBuffer, Timestamp)> {
    let buffer = TransformBuffer::new(
        TfResourceConfig::default(),
        LookupPolicy {
            max_timestamp_uncertainty_ns: None,
            max_interpolation_gap: Duration::from_millis(100),
            allow_unknown_uncertainty: false,
        },
    )?;
    let authority = buffer.bind_provider("history_provider", CapabilityInstanceId::new())?;
    let now = clock.monotonic_now()?;
    let start = now.nanoseconds().saturating_sub(100_000_000);
    for (delta, x) in [(0_i64, 0.0), (50_000_000, 10.0)] {
        buffer.ingest_dynamic(
            &authority,
            DynamicRecord {
                parent: rbt_tf::FrameId::new("history_parent")?,
                child: rbt_tf::FrameId::new("history_child")?,
                stamp: Timestamp::new(*clock.monotonic_identity(), start.saturating_add(delta)),
                uncertainty_ns: Some(0),
                transform_epoch: 1,
                transform: translated(x, 0.0, 0.0)?,
            },
        )?;
    }
    let query = Timestamp::new(
        *clock.monotonic_identity(),
        start.saturating_add(25_000_000),
    );
    Ok((buffer, query))
}

fn timed_queries<F>(queries: usize, mut query: F) -> Result<Vec<u64>>
where
    F: FnMut() -> Result<()>,
{
    let mut latencies = Vec::with_capacity(queries);
    let start = Instant::now();
    for _ in 0..queries {
        let one = Instant::now();
        query()?;
        latencies.push(u64::try_from(one.elapsed().as_nanos()).unwrap_or(u64::MAX));
    }
    let elapsed = start.elapsed();
    if elapsed.is_zero() {
        bail!("benchmark timer returned zero elapsed duration");
    }
    Ok(latencies)
}

fn latency_json(
    name: String,
    queries: usize,
    mut values: Vec<u64>,
    rss_before_kib: u64,
    rss_after_kib: u64,
    kind: &str,
) -> Value {
    values.sort_unstable();
    let percentile = |percent: usize| -> u64 {
        let index = (values.len().saturating_sub(1) * percent).div_ceil(100);
        values[index.min(values.len() - 1)]
    };
    let total_ns: u128 = values.iter().map(|value| u128::from(*value)).sum();
    let rate = if total_ns == 0 {
        0.0
    } else {
        queries as f64 * 1.0e9 / total_ns as f64
    };
    json!({
        "name": name,
        "measurement_kind": kind,
        "transport_included": false,
        "correct_results": queries,
        "queries": queries,
        "p50_latency_ns": percentile(50),
        "p90_latency_ns": percentile(90),
        "p99_latency_ns": percentile(99),
        "max_latency_ns": values.last().copied().unwrap_or(0),
        "queries_per_second": rate,
        "rss_before_kib": rss_before_kib,
        "rss_after_kib": rss_after_kib,
        "rss_growth_kib": rss_after_kib.saturating_sub(rss_before_kib),
    })
}

fn rss_kib() -> Option<u64> {
    let status = fs::read_to_string("/proc/self/status").ok()?;
    status.lines().find_map(|line| {
        line.strip_prefix("VmRSS:")
            .and_then(|value| value.split_whitespace().next())
            .and_then(|value| value.parse::<u64>().ok())
    })
}

fn run_composer(options: BTreeMap<String, String>) -> Result<()> {
    only_options(&options, &["--addr"])?;
    let addr = option(&options, "--addr", DEFAULT_COMPOSER).to_owned();
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()?;
    runtime.block_on(async move {
        tokio::select! {
            result = rbt_compose::serve(addr) => result?,
            signal = process_shutdown_signal() => signal?,
        }
        report_process_shutdown_complete("composer")?;
        Ok::<_, anyhow::Error>(())
    })
}

fn run_provider(options: BTreeMap<String, String>) -> Result<()> {
    only_options(&options, &["--addr"])?;
    let addr = option(&options, "--addr", DEFAULT_COMPOSER).to_owned();
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()?;
    runtime.block_on(async move {
        run_component::<Provider, _>((), addr).await?;
        report_process_shutdown_complete("provider")?;
        Ok::<_, anyhow::Error>(())
    })
}

fn run_consumer(options: BTreeMap<String, String>) -> Result<()> {
    only_options(&options, &["--addr", "--id", "--slow-drain-ms"])?;
    let addr = option(&options, "--addr", DEFAULT_COMPOSER).to_owned();
    let id = option(&options, "--id", "tf-software-consumer-0").to_owned();
    let slow_drain_ms = option(&options, "--slow-drain-ms", "0")
        .parse::<u64>()
        .context("--slow-drain-ms must be a nonnegative integer")?;
    if slow_drain_ms > 1000 {
        bail!("--slow-drain-ms must be in 0..=1000");
    }
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()?;
    runtime.block_on(async move {
        run_component::<Consumer, _>(ConsumerConfig { id, slow_drain_ms }, addr).await?;
        report_process_shutdown_complete("consumer")?;
        Ok::<_, anyhow::Error>(())
    })
}

async fn process_shutdown_signal() -> std::io::Result<()> {
    #[cfg(unix)]
    {
        let mut sigint = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt())?;
        let mut sigterm =
            tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
        tokio::select! {
            _ = sigint.recv() => Ok(()),
            _ = sigterm.recv() => Ok(()),
        }
    }
    #[cfg(not(unix))]
    {
        tokio::signal::ctrl_c().await
    }
}

fn report_process_shutdown_complete(role: &str) -> Result<()> {
    println!("{}", json!({"event": "shutdown_complete", "role": role}));
    flush_stdout()
}

struct Provider;

#[derive(Clone, Copy)]
struct NativePressureIds {
    dynamic: CapabilityInstanceId,
    static_update: CapabilityInstanceId,
    static_snapshot: CapabilityInstanceId,
}

fn native_transport_pressure(ids: &NativePressureIds) -> Value {
    let stream_names = [
        ids.dynamic.stream_service_name(),
        ids.static_update.stream_service_name(),
    ];
    let rpc_name = ids.static_snapshot.rpc_service_name();
    let streams: Vec<_> = rbt_core::local_pressure_snapshots()
        .into_iter()
        .filter(|snapshot| {
            stream_names
                .iter()
                .any(|name| name == &snapshot.service_name)
        })
        .collect();
    let rpc: Vec<_> = rbt_core::local_rpc_pressure_snapshots()
        .into_iter()
        .filter(|snapshot| {
            snapshot.capability_instance == Some(ids.static_snapshot)
                && snapshot.service_name == rpc_name
        })
        .collect();
    json!({
        "capability_ids": {
            "dynamic": ids.dynamic,
            "static_update": ids.static_update,
            "static_snapshot": ids.static_snapshot,
        },
        "streams": streams,
        "rpc": rpc,
    })
}

#[async_trait]
impl Component for Provider {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("tf-software-provider")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let clock = HostClock::new()?;
        let effective_from = clock.monotonic_now()?;
        let static_set = StaticSet {
            revision: 1,
            effective_from,
            effective_uncertainty_ns: None,
            transforms: vec![StaticEdge {
                parent: rbt_tf::FrameId::new("base_link")?,
                child: rbt_tf::FrameId::new("camera_link")?,
                transform: translated(1.0, 0.0, 0.0)?,
            }],
        };
        let static_tf = ctx.provide_static_tf::<StaticUpdateRole, StaticSnapshotRole>(
            static_set,
            MAX_STATIC_TRANSFORMS,
            StreamProfile::Control,
        )?;
        let dynamic = ctx.provide_dynamic_tf::<DynamicRole>(StreamProfile::Control)?;
        let pressure_ids = NativePressureIds {
            dynamic: dynamic.capability,
            static_update: static_tf.update_authority,
            static_snapshot: static_tf.snapshot_capability,
        };
        let activation = ctx.activation_id();
        println!(
            "{}",
            serde_json::to_string(&json!({
                "event": "activation",
                "role": "provider",
                "activation_id": activation.value(),
                "fixture": FIXTURE_NOTICE,
                "static_update_id": static_tf.update_authority,
                "static_snapshot_id": static_tf.snapshot_capability,
                "dynamic_id": dynamic.capability,
                "dynamic_uncertainty_policy": "unknown precision preserved",
            }))?
        );
        flush_stdout()?;
        let publisher = dynamic.publisher;
        let static_control = static_tf.control;
        let child_clock = HostClock::new()?;
        ctx.spawn_critical("tf-software-provider", move |token| async move {
            provider_loop(publisher, static_control, child_clock, pressure_ids, token).await
        });
        Ok(())
    }
}

async fn provider_loop(
    publisher: DynamicTfPublisher,
    static_control: rbt_tf_transport::StaticTfProviderControl,
    clock: HostClock,
    pressure_ids: NativePressureIds,
    token: CancellationToken,
) -> Result<()> {
    let mut publish_tick = tokio::time::interval(Duration::from_millis(10));
    publish_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut report_tick = tokio::time::interval(Duration::from_secs(1));
    report_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut static_tick = tokio::time::interval(Duration::from_secs(2));
    static_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut samples = 0_u64;
    let mut queue_full = 0_u64;
    let mut updates = 0_u64;
    loop {
        tokio::select! {
            _ = token.cancelled() => {
                let dynamic_pressure = publisher.pressure_snapshot()?;
                let static_pressure = static_control.update_pressure_snapshot()?;
                let dynamic_loans = dynamic_pressure.publisher
                    .ok_or_else(|| anyhow!("dynamic provider pressure snapshot has no publisher metrics"))?
                    .loans_in_use;
                let static_loans = static_pressure.publisher
                    .ok_or_else(|| anyhow!("static provider pressure snapshot has no publisher metrics"))?
                    .loans_in_use;
                let retained_loans = dynamic_loans + static_loans;
                if retained_loans != 0 {
                    bail!("provider deactivated with {retained_loans} native sample loans retained");
                }
                println!("{}", json!({ "event": "deactivation", "role": "provider", "dynamic_samples": samples, "dynamic_queue_full": queue_full, "static_updates": updates, "transport_pressure": native_transport_pressure(&pressure_ids), "retained_loans": retained_loans }));
                flush_stdout()?;
                return Ok(());
            }
            _ = publish_tick.tick() => {
                let stamp = clock.monotonic_now()?;
                let translation = stamp.nanoseconds() as f64 * 1.0e-9;
                match publisher.publish(&DynamicRecord {
                    parent: rbt_tf::FrameId::new("odom")?,
                    child: rbt_tf::FrameId::new("base_link")?,
                    stamp,
                    uncertainty_ns: None,
                    transform_epoch: 1,
                    transform: translated(translation, 0.0, 0.0)?,
                }) {
                    Ok(_) => samples = samples.saturating_add(1),
                    Err(rbt_tf_transport::TransportError::Stream(rbt_iox2::StreamError::QueueFull)) => {
                        queue_full = queue_full.saturating_add(1);
                    }
                    Err(error) => return Err(error.into()),
                }
            }
            _ = static_tick.tick() => {
                let current = static_control.current()?;
                static_control.replace(StaticSet {
                    revision: current.revision.checked_add(1).ok_or_else(|| anyhow!("static revision overflow"))?,
                    effective_from: clock.monotonic_now()?,
                    effective_uncertainty_ns: None,
                    transforms: current.transforms,
                })?;
                updates = updates.saturating_add(1);
            }
            _ = report_tick.tick() => {
                println!("{}", json!({ "event": "provider_summary", "role": "provider", "dynamic_samples": samples, "dynamic_queue_full": queue_full, "static_updates": updates, "transport_pressure": native_transport_pressure(&pressure_ids), "fixture": FIXTURE_NOTICE }));
                flush_stdout()?;
            }
        }
    }
}

#[derive(Clone)]
struct ConsumerConfig {
    id: String,
    slow_drain_ms: u64,
}

#[derive(Clone, Default)]
struct SlowSubscriberSnapshot {
    delay_ms: u64,
    received: u64,
    accepted: u64,
    rejected: u64,
    stream: Value,
    buffer: Value,
}

struct Consumer {
    id: String,
    slow_drain_ms: u64,
}

#[async_trait]
impl Component for Consumer {
    type Config = ConsumerConfig;
    type Error = anyhow::Error;

    async fn prepare(config: &Self::Config) -> Result<Self> {
        Ok(Self {
            id: config.id.clone(),
            slow_drain_ms: config.slow_drain_ms,
        })
    }

    fn id(&self) -> ComponentId {
        ComponentId::from(self.id.clone())
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        vec![
            RequirementSpec::typed::<StaticUpdateRole>("tf_static_updates"),
            RequirementSpec::typed::<StaticSnapshotRole>("tf_static_snapshot"),
            RequirementSpec::typed::<DynamicRole>("tf_dynamic"),
        ]
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let buffer = TransformBuffer::new(
            TfResourceConfig::default(),
            LookupPolicy {
                max_timestamp_uncertainty_ns: None,
                max_interpolation_gap: Duration::from_millis(100),
                allow_unknown_uncertainty: true,
            },
        )?;
        let activation_id = ctx.activation_id().value();
        let pressure_ids = NativePressureIds {
            dynamic: ctx
                .binding::<DynamicRole>("tf_dynamic")?
                .descriptor()
                .capability_instance(),
            static_update: ctx
                .binding::<StaticUpdateRole>("tf_static_updates")?
                .descriptor()
                .capability_instance(),
            static_snapshot: ctx
                .binding::<StaticSnapshotRole>("tf_static_snapshot")?
                .descriptor()
                .capability_instance(),
        };
        ctx.scope().defer(move || {
            let transport_pressure = native_transport_pressure(&pressure_ids);
            let all_sources_released = transport_pressure
                .get("streams")
                .and_then(Value::as_array)
                .is_some_and(Vec::is_empty)
                && transport_pressure
                    .get("rpc")
                    .and_then(Value::as_array)
                    .is_some_and(Vec::is_empty);
            println!(
                "{}",
                json!({
                    "event": "activation_transport_retired",
                    "role": "consumer",
                    "activation_id": activation_id,
                    "transport_pressure": transport_pressure,
                    "all_sources_released": all_sources_released,
                })
            );
            let _ = flush_stdout();
        });
        ctx.require_static_tf::<StaticUpdateRole, StaticSnapshotRole>(
            "tf_static_updates",
            "tf_static_snapshot",
            "software_static",
            MAX_STATIC_TRANSFORMS,
            buffer.clone(),
        )?;
        ctx.require_dynamic_tf::<DynamicRole>("tf_dynamic", "software_dynamic", buffer.clone())?;
        let slow_diagnostics = if self.slow_drain_ms == 0 {
            None
        } else {
            let binding = ctx.binding::<DynamicRole>("tf_dynamic")?;
            let descriptor = binding
                .try_stream()
                .ok_or_else(|| anyhow!("slow subscriber dynamic binding is not a stream"))?
                .clone();
            let slow_buffer = TransformBuffer::new(
                TfResourceConfig::default(),
                LookupPolicy {
                    max_timestamp_uncertainty_ns: None,
                    max_interpolation_gap: Duration::from_millis(100),
                    allow_unknown_uncertainty: true,
                },
            )?;
            let subscriber = DynamicTfSubscriber::open(
                &descriptor,
                binding.descriptor().capability_instance(),
                "slow_native_dynamic",
                slow_buffer.clone(),
            )?;
            let wait = WaitContext::new(ctx.process_instance(), ctx.activation())?;
            let wake = wait.wake_handle();
            ctx.scope().wake_on_shutdown(move || {
                let _ = wake.wake();
            });
            let task_wake = wait.wake_handle();
            let diagnostics = Arc::new(Mutex::new(SlowSubscriberSnapshot {
                delay_ms: self.slow_drain_ms,
                ..SlowSubscriberSnapshot::default()
            }));
            let task_diagnostics = Arc::clone(&diagnostics);
            let delay = Duration::from_millis(self.slow_drain_ms);
            ctx.spawn_critical_blocking(
                "tf-slow-native-dynamic-subscriber",
                move |cancellation| {
                    ingest_slow_dynamic_stream(
                        subscriber,
                        wait,
                        slow_buffer,
                        delay,
                        task_diagnostics,
                        cancellation,
                    )
                },
                move || {
                    let _ = task_wake.wake();
                },
            );
            Some(diagnostics)
        };
        let clock = HostClock::new()?;
        println!(
            "{}",
            json!({
                "event": "activation",
                "role": "consumer",
                "activation_id": ctx.activation_id().value(),
                "static_physical_id": pressure_ids.static_update,
                "static_snapshot_physical_id": pressure_ids.static_snapshot,
                "dynamic_physical_id": pressure_ids.dynamic,
                "fixture": FIXTURE_NOTICE,
                "buffers": 1,
                "query_mode": "local lookup at host monotonic now minus 50ms",
                "slow_native_subscriber": { "enabled": self.slow_drain_ms > 0, "drain_delay_ms": self.slow_drain_ms },
            })
        );
        flush_stdout()?;
        ctx.spawn_critical("tf-software-consumer", move |token| async move {
            consumer_loop(
                buffer,
                clock,
                activation_id,
                pressure_ids,
                slow_diagnostics,
                token,
            )
            .await
        });
        Ok(())
    }
}

async fn consumer_loop(
    buffer: TransformBuffer,
    clock: HostClock,
    activation_id: u64,
    pressure_ids: NativePressureIds,
    slow_diagnostics: Option<Arc<Mutex<SlowSubscriberSnapshot>>>,
    token: CancellationToken,
) -> Result<()> {
    let mut tick = tokio::time::interval(Duration::from_secs(1));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    tick.tick().await;
    let mut attempts = 0_u64;
    let mut successes = 0_u64;
    let mut verified = 0_u64;
    let mut failures = 0_u64;
    let mut last_error = None::<String>;
    loop {
        tokio::select! {
            _ = token.cancelled() => {
                println!("{}", json!({ "event": "deactivation", "role": "consumer", "activation_id": activation_id, "lookup_attempts": attempts, "lookup_successes": successes, "correct_results": verified, "lookup_errors": failures, "last_error": last_error, "buffer": buffer.pressure_snapshot().ok(), "transport_pressure": native_transport_pressure(&pressure_ids), "slow_native_subscriber": slow_subscriber_pressure(&slow_diagnostics) }));
                flush_stdout()?;
                return Ok(());
            }
            _ = tick.tick() => {
                attempts = attempts.saturating_add(1);
                let query = clock.monotonic_now()?.checked_add_nanoseconds(-50_000_000)?;
                match buffer.lookup_at_with("odom", "camera_link", query, LookupOptions { timestamp_uncertainty_ns: None, mappings: &[], now: None }) {
                    Ok(result) => {
                        successes = successes.saturating_add(1);
                        let expected_x = query.nanoseconds() as f64 * 1.0e-9 + 1.0;
                        let valid = (result.transform.translation().x() - expected_x).abs() < 2.0e-5
                            && result.timing_uncertainty_ns.is_none()
                            && result.sources.iter().any(|source| source.transform_epoch == Some(1));
                        if valid { verified = verified.saturating_add(1); }
                        else { failures = failures.saturating_add(1); last_error = Some("synthetic result did not match its identified input time".to_owned()); }
                    }
                    Err(error) => { failures = failures.saturating_add(1); last_error = Some(format!("{error:?}")); }
                }
                println!("{}", json!({ "event": "consumer_summary", "role": "consumer", "activation_id": activation_id, "lookup_attempts": attempts, "lookup_successes": successes, "correct_results": verified, "lookup_errors": failures, "last_error": last_error, "buffer": buffer.pressure_snapshot().ok(), "transport_pressure": native_transport_pressure(&pressure_ids), "slow_native_subscriber": slow_subscriber_pressure(&slow_diagnostics), "fixture": FIXTURE_NOTICE }));
                flush_stdout()?;
            }
        }
    }
}

fn slow_subscriber_pressure(diagnostics: &Option<Arc<Mutex<SlowSubscriberSnapshot>>>) -> Value {
    let Some(diagnostics) = diagnostics else {
        return json!({ "enabled": false });
    };
    let snapshot = diagnostics
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .clone();
    json!({
        "enabled": true,
        "drain_delay_ms": snapshot.delay_ms,
        "received_samples": snapshot.received,
        "accepted_samples": snapshot.accepted,
        "rejected_samples": snapshot.rejected,
        "stream": snapshot.stream,
        "buffer": snapshot.buffer,
    })
}

fn ingest_slow_dynamic_stream(
    subscriber: DynamicTfSubscriber,
    mut wait: WaitContext,
    buffer: TransformBuffer,
    delay: Duration,
    diagnostics: Arc<Mutex<SlowSubscriberSnapshot>>,
    cancellation: CancellationToken,
) -> rbt_tf_transport::TransportResult<()> {
    let source = subscriber.event_source(WaitToken::new(1));
    let sources = [source];
    while !cancellation.is_cancelled() {
        let result = wait.run(&sources, |_, budget| {
            thread::sleep(delay);
            let report = subscriber.drain_bounded(budget)?;
            let mut snapshot = diagnostics
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            snapshot.received = snapshot.received.saturating_add(report.received as u64);
            snapshot.accepted = snapshot.accepted.saturating_add(report.accepted as u64);
            snapshot.rejected = snapshot.rejected.saturating_add(report.rejected as u64);
            snapshot.stream = json!(subscriber.pressure_snapshot());
            snapshot.buffer = buffer
                .pressure_snapshot()
                .map(|pressure| json!(pressure))
                .unwrap_or(Value::Null);
            Ok::<_, rbt_tf_transport::TransportError>(WaitProgress::samples(report.received))
        })?;
        if cancellation.is_cancelled() || wait.is_cancelled() || result.is_cancelled() {
            return Ok(());
        }
    }
    Ok(())
}

fn interop_publish(options: BTreeMap<String, String>) -> Result<()> {
    let args = interop_args(options)?;
    remove_file_if_present(&args.ready)?;
    remove_file_if_present(&args.done)?;
    let (static_id, dynamic_id, snapshot_id) = args.ids;
    let update_descriptor = SliceStream::<StaticUpdate>::descriptor(
        static_id.stream_service_name(),
        StreamProfile::Control,
    );
    let dynamic_descriptor = Stream::<DynamicTransform>::descriptor(
        dynamic_id.stream_service_name(),
        StreamProfile::Control,
    );
    let rpc_resources = RpcResourceConfig {
        max_response_slice_len: 64,
        ..RpcResourceConfig::default()
    };
    let snapshot_descriptor = RpcDescriptor::for_fixed_slice_with_resources::<
        StaticSnapshotRequest,
        StaticSnapshot,
    >(snapshot_id.rpc_service_name(), rpc_resources);
    let device = DeviceClock::new([42; 16], 7)?;
    let initial = interop_static_set(&device)?;
    let mut provider = StaticTfProvider::open(
        &update_descriptor,
        &snapshot_descriptor,
        static_id,
        snapshot_id,
        initial,
        64,
    )?;
    let publisher = DynamicTfPublisher::open(&dynamic_descriptor, dynamic_id)?;
    write_marker(&args.ready)?;
    let deadline = Instant::now() + args.timeout;
    wait_for_file(&args.peer_ready, deadline)?;

    let mut replacement = interop_static_set(&device)?;
    replacement.revision = 11;
    provider.replace(replacement)?;

    let mut sent = 0_u64;
    for (ns, x, rotation) in [
        (100_000_000, 0.0, Quaternion::identity()),
        (
            200_000_000,
            2.0,
            Quaternion::new(
                0.0,
                0.0,
                std::f64::consts::FRAC_1_SQRT_2,
                std::f64::consts::FRAC_1_SQRT_2,
            )?,
        ),
    ] {
        publisher.publish(&DynamicRecord {
            parent: rbt_tf::FrameId::new("odom")?,
            child: rbt_tf::FrameId::new("base_link")?,
            stamp: device.stamp(ns),
            uncertainty_ns: Some(0),
            transform_epoch: 3,
            transform: Transform3::new(Vec3::new(x, 0.0, 0.0)?, rotation),
        })?;
        sent += 1;
        thread::sleep(Duration::from_millis(25));
    }

    let mut snapshot_responses = 0_u64;
    loop {
        if args.done.exists() {
            let elapsed = deadline.saturating_duration_since(Instant::now());
            let dynamic_pressure = publisher.pressure_snapshot()?;
            let static_update_pressure = provider.control().update_pressure_snapshot()?;
            let snapshot_rpc_pressure = provider.snapshot_rpc_pressure();
            let dynamic_metrics = dynamic_pressure.publisher.ok_or_else(|| {
                anyhow!("dynamic publisher pressure snapshot has no publisher metrics")
            })?;
            let static_metrics = static_update_pressure.publisher.ok_or_else(|| {
                anyhow!("static update pressure snapshot has no publisher metrics")
            })?;
            let snapshot_metrics = snapshot_rpc_pressure
                .server
                .ok_or_else(|| anyhow!("snapshot RPC pressure snapshot has no server metrics"))?;
            let retained_loans = dynamic_metrics.loans_in_use + static_metrics.loans_in_use;
            if sent != 2
                || dynamic_metrics.send_successes < sent
                || dynamic_metrics.loans_in_use != 0
                || static_metrics.loans_in_use != 0
                || snapshot_responses == 0
                || snapshot_metrics.responses_sent < snapshot_responses
                || snapshot_metrics.requests_current != 0
            {
                bail!(
                    "interop publisher native pressure did not settle: sent={sent}, dynamic={dynamic_metrics:?}, static={static_metrics:?}, snapshot={snapshot_metrics:?}, snapshot_responses={snapshot_responses}"
                );
            }
            println!(
                "{}",
                json!({
                    "mode": "interop.publish",
                    "status": "PASS",
                    "language": "rust",
                    "static_snapshot": "PASS",
                    "static_update": "PASS",
                    "dynamic": "PASS",
                    "lookup": "PASS",
                    "lookup_evidence": "peer consumer completed its verified lookup before the done marker",
                    "fixture": FIXTURE_NOTICE,
                    "static_authority": static_id,
                    "dynamic_authority": dynamic_id,
                    "snapshot_capability": snapshot_id,
                    "dynamic_samples": sent,
                    "static_revision": 11,
                    "snapshot_responses": snapshot_responses,
                    "retained_loans": retained_loans,
                    "retained_sample_guards": retained_loans + snapshot_metrics.requests_current,
                    "borrowed_samples": 0,
                    "borrowed_samples_scope": "publisher process has no stream subscriber; RPC request guards are counted separately",
                    "pending_rpc": snapshot_metrics.requests_current,
                    "native_pressure": {
                        "dynamic_publisher": dynamic_pressure,
                        "static_update_publisher": static_update_pressure,
                        "snapshot_rpc_server": snapshot_rpc_pressure,
                    },
                    "remaining_timeout_ms": elapsed.as_millis(),
                })
            );
            return Ok(());
        }
        if Instant::now() >= deadline {
            bail!("interop publisher timed out waiting for done marker");
        }
        if let rbt_tf_transport::StaticServeOutcome::Served { .. } =
            provider.serve_one(Duration::from_millis(50))?
        {
            snapshot_responses = snapshot_responses.saturating_add(1);
        }
    }
}

fn interop_consume(options: BTreeMap<String, String>) -> Result<()> {
    let args = interop_args(options)?;
    remove_file_if_present(&args.peer_ready)?;
    remove_file_if_present(&args.done)?;
    let (static_id, dynamic_id, snapshot_id) = args.ids;
    let deadline = Instant::now() + args.timeout;
    wait_for_file(&args.ready, deadline)?;

    let update_descriptor = SliceStream::<StaticUpdate>::descriptor(
        static_id.stream_service_name(),
        StreamProfile::Control,
    );
    let dynamic_descriptor = Stream::<DynamicTransform>::descriptor(
        dynamic_id.stream_service_name(),
        StreamProfile::Control,
    );
    let rpc_resources = RpcResourceConfig {
        max_response_slice_len: 64,
        ..RpcResourceConfig::default()
    };
    let snapshot_descriptor = RpcDescriptor::for_fixed_slice_with_resources::<
        StaticSnapshotRequest,
        StaticSnapshot,
    >(snapshot_id.rpc_service_name(), rpc_resources);
    let device = DeviceClock::new([42; 16], 7)?;
    let buffer = TransformBuffer::new(
        TfResourceConfig::default(),
        LookupPolicy {
            max_timestamp_uncertainty_ns: Some(0),
            max_interpolation_gap: Duration::from_millis(100),
            allow_unknown_uncertainty: false,
        },
    )?;
    // `StaticTfClient::open` creates the update subscriber before its RPC client.
    let static_client = StaticTfClient::open(
        static_id,
        &update_descriptor,
        &snapshot_descriptor,
        snapshot_id,
        ActivationId::first(),
        "interop_static",
        64,
        buffer.clone(),
    )?;
    let dynamic_subscriber = DynamicTfSubscriber::open(
        &dynamic_descriptor,
        dynamic_id,
        "interop_dynamic",
        buffer.clone(),
    )?;
    write_marker(&args.peer_ready)?;

    let mut update_received = 0_usize;
    let mut update_accepted = 0_usize;
    while update_received == 0 {
        if Instant::now() >= deadline {
            bail!("interop consumer timed out waiting for the static update stream");
        }
        let report = static_client.drain_updates(64)?;
        update_received += report.received;
        update_accepted += report.accepted;
        if report.rejected != 0 {
            bail!(
                "interop consumer rejected {} static updates: {:?}",
                report.rejected,
                report.failures
            );
        }
        if report.received == 0 {
            thread::sleep(Duration::from_millis(2));
        }
    }
    if update_accepted == 0 {
        bail!("interop static update was received but not accepted");
    }
    static_client.refresh_snapshot(Duration::from_secs(2))?;
    let mut dynamic_received = 0_usize;
    let mut dynamic_accepted = 0_usize;
    while dynamic_accepted < 2 {
        if Instant::now() >= deadline {
            bail!("interop consumer timed out waiting for dynamic records");
        }
        let report = dynamic_subscriber.drain_bounded(64)?;
        dynamic_received += report.received;
        dynamic_accepted += report.accepted;
        if report.rejected != 0 {
            bail!(
                "interop consumer rejected {} dynamic records: {:?}",
                report.rejected,
                report.failures
            );
        }
        if report.received == 0 {
            thread::sleep(Duration::from_millis(2));
        }
    }

    let query = device.stamp(150_000_000);
    let result = buffer.lookup_at_with(
        "odom",
        "camera_optical",
        query,
        LookupOptions {
            timestamp_uncertainty_ns: Some(0),
            mappings: &[],
            now: None,
        },
    )?;
    let point = result.apply_point(Vec3::zero())?;
    let expected = 1.0 + std::f64::consts::FRAC_1_SQRT_2;
    if (point.x() - expected).abs() > 1.0e-9
        || (point.y() - std::f64::consts::FRAC_1_SQRT_2).abs() > 1.0e-9
        || point.z().abs() > 1.0e-9
    {
        bail!(
            "interop transform direction/math mismatch: got {:?}",
            point.xyz()
        );
    }
    let round_trip = result.transform.inverse()?.apply_point(point)?;
    let (roundtrip_x, roundtrip_y, roundtrip_z) = round_trip.xyz();
    if [roundtrip_x, roundtrip_y, roundtrip_z]
        .iter()
        .any(|value| value.abs() > 1.0e-9)
    {
        bail!("interop inverse roundtrip failed: {:?}", round_trip.xyz());
    }
    let dynamic_source = result
        .sources
        .iter()
        .find(|source| source.transform_epoch == Some(3))
        .ok_or_else(|| anyhow!("dynamic source/epoch provenance is missing"))?;
    let static_source = result
        .sources
        .iter()
        .find(|source| source.static_revision == Some(11))
        .ok_or_else(|| anyhow!("static revision 11 provenance is missing"))?;
    let static_edges: Vec<_> = result
        .sources
        .iter()
        .filter(|source| source.static_revision == Some(11))
        .map(|source| (source.parent.as_str(), source.child.as_str()))
        .collect();
    if result.requested_timestamp != query
        || dynamic_source.timestamp.clock() != query.clock()
        || dynamic_source.timestamp.nanoseconds() != query.nanoseconds()
        || static_source.timestamp.clock() != query.clock()
        || dynamic_source.parent.as_str() != "odom"
        || dynamic_source.child.as_str() != "base_link"
        || static_edges.len() != 2
        || !static_edges.contains(&("base_link", "camera_link"))
        || !static_edges.contains(&("camera_link", "camera_optical"))
        || query.clock().generation() != 7
        || query.clock().source_id() != ClockSourceId::from_device_id([42; 16])?
    {
        bail!("interop time, frame, epoch, or source identity provenance mismatch");
    }
    let dynamic_pressure = dynamic_subscriber.pressure_snapshot();
    let static_update_pressure = static_client.update_pressure_snapshot();
    let snapshot_rpc_pressure = static_client.snapshot_rpc_pressure();
    let dynamic_borrowed = dynamic_pressure
        .subscriber
        .ok_or_else(|| anyhow!("dynamic subscriber pressure snapshot has no subscriber metrics"))?
        .borrowed_samples;
    let static_borrowed = static_update_pressure
        .subscriber
        .ok_or_else(|| {
            anyhow!("static update subscriber pressure snapshot has no subscriber metrics")
        })?
        .borrowed_samples;
    let rpc_client = snapshot_rpc_pressure
        .client
        .ok_or_else(|| anyhow!("snapshot RPC pressure snapshot has no client metrics"))?;
    let borrowed_samples = dynamic_borrowed + static_borrowed;
    if borrowed_samples != 0
        || rpc_client.in_flight_current != 0
        || rpc_client.response_guards_current != 0
    {
        bail!(
            "interop consumer retained native resources: dynamic_borrowed={dynamic_borrowed}, static_borrowed={static_borrowed}, rpc={rpc_client:?}"
        );
    }
    write_marker(&args.done)?;
    let summary = json!({
        "mode": "interop.consume",
        "status": "PASS",
        "language": "rust",
        "static_snapshot": "PASS",
        "static_update": "PASS",
        "dynamic": "PASS",
        "lookup": "PASS",
        "fixture": FIXTURE_NOTICE,
        "query": { "clock_kind": "device", "source_id": query.clock().source_id().to_hex(), "generation": query.clock().generation(), "nanoseconds": query.nanoseconds() },
        "point_from_camera_optical_origin": vector(point),
        "expected_point": [expected, std::f64::consts::FRAC_1_SQRT_2, 0.0],
        "inverse_roundtrip_origin": vector(round_trip),
        "dynamic_epoch": dynamic_source.transform_epoch,
        "dynamic_frames": [dynamic_source.parent.as_str(), dynamic_source.child.as_str()],
        "static_revision": static_source.static_revision,
        "static_update_records_received": update_received,
        "static_update_records_accepted": update_accepted,
        "static_frames": static_edges,
        "dynamic_samples": dynamic_accepted,
        "dynamic_records_received": dynamic_received,
        "buffer": buffer.pressure_snapshot()?,
        "retained_sample_guards": borrowed_samples,
        "retained_rpc_response_guards": rpc_client.response_guards_current,
        "borrowed_samples": borrowed_samples,
        "pending_rpc": rpc_client.in_flight_current,
        "native_pressure": {
            "dynamic_subscriber": dynamic_pressure,
            "static_update_subscriber": static_update_pressure,
            "snapshot_rpc_client": snapshot_rpc_pressure,
        },
    });
    println!("{}", serde_json::to_string(&summary)?);
    Ok(())
}

fn interop_static_set(device: &DeviceClock) -> Result<StaticSet> {
    Ok(StaticSet {
        revision: 10,
        effective_from: device.stamp(0),
        effective_uncertainty_ns: Some(0),
        transforms: vec![
            StaticEdge {
                parent: rbt_tf::FrameId::new("base_link")?,
                child: rbt_tf::FrameId::new("camera_link")?,
                transform: translated(1.0, 0.0, 0.0)?,
            },
            StaticEdge {
                parent: rbt_tf::FrameId::new("camera_link")?,
                child: rbt_tf::FrameId::new("camera_optical")?,
                transform: Transform3::identity(),
            },
        ],
    })
}

struct InteropArgs {
    ids: (
        CapabilityInstanceId,
        CapabilityInstanceId,
        CapabilityInstanceId,
    ),
    ready: PathBuf,
    peer_ready: PathBuf,
    done: PathBuf,
    timeout: Duration,
}

fn interop_args(options: BTreeMap<String, String>) -> Result<InteropArgs> {
    only_options(
        &options,
        &[
            "--static-id",
            "--dynamic-id",
            "--snapshot-id",
            "--ready",
            "--peer-ready",
            "--done",
            "--timeout-sec",
        ],
    )?;
    let id = |name: &str| -> Result<CapabilityInstanceId> {
        let value = options
            .get(name)
            .ok_or_else(|| anyhow!("missing required {name}"))?;
        Ok(CapabilityInstanceId::from_uuid(
            Uuid::parse_str(value).with_context(|| format!("invalid UUID for {name}"))?,
        ))
    };
    let path = |name: &str| -> Result<PathBuf> {
        options
            .get(name)
            .map(PathBuf::from)
            .ok_or_else(|| anyhow!("missing required {name}"))
    };
    let seconds = options
        .get("--timeout-sec")
        .ok_or_else(|| anyhow!("missing required --timeout-sec"))?
        .parse::<u64>()
        .context("--timeout-sec must be a positive integer")?;
    if seconds == 0 || seconds > 3600 {
        bail!("--timeout-sec must be in 1..=3600");
    }
    let ids = (
        id("--static-id")?,
        id("--dynamic-id")?,
        id("--snapshot-id")?,
    );
    if ids.0.as_uuid().is_nil()
        || ids.1.as_uuid().is_nil()
        || ids.2.as_uuid().is_nil()
        || ids.0 == ids.1
        || ids.0 == ids.2
        || ids.1 == ids.2
    {
        bail!("interop capability IDs must be non-nil and pairwise distinct");
    }
    Ok(InteropArgs {
        ids,
        ready: path("--ready")?,
        peer_ready: path("--peer-ready")?,
        done: path("--done")?,
        timeout: Duration::from_secs(seconds),
    })
}

fn wait_for_file(path: &Path, deadline: Instant) -> Result<()> {
    loop {
        if path.is_file() {
            return Ok(());
        }
        if Instant::now() >= deadline {
            bail!("timed out waiting for readiness file {}", path.display());
        }
        thread::sleep(Duration::from_millis(10));
    }
}

fn write_marker(path: &PathBuf) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(path, b"ready\n")
        .with_context(|| format!("write readiness marker {}", path.display()))
}

fn remove_file_if_present(path: &PathBuf) -> Result<()> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error).with_context(|| format!("remove stale marker {}", path.display())),
    }
}

fn translated(x: f64, y: f64, z: f64) -> Result<Transform3> {
    Ok(Transform3::new(Vec3::new(x, y, z)?, Quaternion::identity()))
}

fn vector(value: Vec3) -> [f64; 3] {
    [value.x(), value.y(), value.z()]
}

fn flush_stdout() -> Result<()> {
    std::io::stdout().flush()?;
    Ok(())
}

fn quaternion(value: Quaternion) -> [f64; 4] {
    [value.x(), value.y(), value.z(), value.w()]
}

#[cfg(test)]
mod tests {
    use std::{thread, time::Instant};

    use rbt_iox2::{Stream, StreamProfile};
    use rbt_types::DynamicTransform;

    #[test]
    fn native_borrow_pressure_releases_before_endpoint_source_disappears() {
        let service_name = rbt::types::CapabilityInstanceId::new().stream_service_name();
        let descriptor =
            Stream::<DynamicTransform>::descriptor(service_name.clone(), StreamProfile::Control);
        let publisher = Stream::<DynamicTransform>::publisher(&descriptor).unwrap();
        let subscriber = Stream::<DynamicTransform>::subscriber(&descriptor).unwrap();
        let deadline = Instant::now() + std::time::Duration::from_secs(3);
        while publisher
            .pressure()
            .publisher
            .expect("publisher metrics")
            .subscriber_count
            != 1
        {
            assert!(Instant::now() < deadline, "subscriber did not connect");
            thread::yield_now();
        }

        loop {
            let mut sample = publisher.loan().unwrap();
            *sample = DynamicTransform::default();
            if sample.send().unwrap() > 0 {
                break;
            }
            assert!(Instant::now() < deadline, "sample was not delivered");
            thread::yield_now();
        }

        let received = loop {
            if let Some(sample) = subscriber.receive().unwrap() {
                break sample;
            }
            assert!(
                Instant::now() < deadline,
                "published sample was not received"
            );
            thread::yield_now();
        };
        let live = rbt_core::local_pressure_snapshots()
            .into_iter()
            .find(|snapshot| snapshot.service_name == service_name && snapshot.subscriber.is_some())
            .expect("registered native stream source");
        let subscriber_pressure = live.subscriber.expect("subscriber metrics");
        assert_eq!(live.resources.max_borrowed, 1);
        assert_eq!(subscriber_pressure.borrowed_samples, 1);
        assert_eq!(subscriber_pressure.borrowed_high_water, 1);

        drop(received);
        let released = rbt_core::local_pressure_snapshots()
            .into_iter()
            .find(|snapshot| snapshot.service_name == service_name && snapshot.subscriber.is_some())
            .expect("source remains while endpoints are live");
        let subscriber_pressure = released.subscriber.expect("subscriber metrics");
        assert_eq!(subscriber_pressure.borrowed_samples, 0);
        assert_eq!(subscriber_pressure.borrowed_high_water, 1);

        drop(subscriber);
        drop(publisher);
        assert!(
            !rbt_core::local_pressure_snapshots()
                .iter()
                .any(|snapshot| snapshot.service_name == service_name)
        );

        let retained_service_name = rbt::types::CapabilityInstanceId::new().stream_service_name();
        let retained_descriptor = Stream::<DynamicTransform>::descriptor(
            retained_service_name.clone(),
            StreamProfile::Control,
        );
        let retained_publisher =
            Stream::<DynamicTransform>::publisher(&retained_descriptor).unwrap();
        let retained_subscriber =
            Stream::<DynamicTransform>::subscriber(&retained_descriptor).unwrap();
        let retained_deadline = Instant::now() + std::time::Duration::from_secs(3);
        while retained_publisher
            .pressure()
            .publisher
            .expect("publisher metrics")
            .subscriber_count
            != 1
        {
            assert!(
                Instant::now() < retained_deadline,
                "retained-sample subscriber did not connect"
            );
            thread::yield_now();
        }
        let mut sample = retained_publisher.loan().unwrap();
        *sample = DynamicTransform::default();
        assert_eq!(sample.send().unwrap(), 1);
        let retained_sample = loop {
            if let Some(sample) = retained_subscriber.receive().unwrap() {
                break sample;
            }
            assert!(
                Instant::now() < retained_deadline,
                "retained sample was not received"
            );
            thread::yield_now();
        };
        drop(retained_subscriber);
        drop(retained_publisher);
        let after_endpoint_drop = rbt_core::local_pressure_snapshots()
            .into_iter()
            .find(|snapshot| {
                snapshot.service_name == retained_service_name && snapshot.subscriber.is_some()
            })
            .expect("received guard keeps its weakly registered pressure source alive");
        assert_eq!(
            after_endpoint_drop
                .subscriber
                .expect("subscriber metrics")
                .borrowed_samples,
            1
        );
        drop(retained_sample);
        assert!(
            !rbt_core::local_pressure_snapshots()
                .iter()
                .any(|snapshot| snapshot.service_name == retained_service_name)
        );
    }
}

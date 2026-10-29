use std::{
    ops::DerefMut,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

use rbt_compose::{ActivationCtx, EffectScope};
use rbt_core::{
    ActivationId, Capability, CapabilityDescriptor, CapabilityInstanceId, ComponentId,
    ProcessInstanceId, ResolvedBinding, local_pressure_snapshots,
};
use rbt_iox2::{
    FixedSliceRpcServer, RpcDescriptor, RpcResourceConfig, SliceStream, Stream, StreamProfile,
};
use rbt_tf::{
    DynamicRecord, FrameId, LookupOptions, LookupPolicy, Quaternion, StaticEdge, StaticSet,
    TfError, TfResourceConfig, Transform3, TransformBuffer, Vec3, wire,
};
use rbt_tf_transport::{
    DynamicTfSubscriber, StaticTfClient, StaticTfProvider, TransformActivationExt, TransportError,
};
use rbt_time::{ClockIdentity, ClockKind, ClockSourceId, Timestamp};
use rbt_types::{DynamicTransform, StaticSnapshot, StaticSnapshotRequest, StaticUpdate};

struct ServerLoop {
    stop: Arc<AtomicBool>,
    join: Option<thread::JoinHandle<()>>,
}

impl ServerLoop {
    fn start(mut provider: StaticTfProvider) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let thread_stop = Arc::clone(&stop);
        let (ready_tx, ready_rx) = std::sync::mpsc::sync_channel(0);
        let join = thread::spawn(move || {
            ready_tx
                .send(())
                .expect("server startup receiver remains open");
            while !thread_stop.load(Ordering::Acquire) {
                let _ = provider.serve_one(Duration::from_millis(25));
            }
        });
        ready_rx.recv().expect("server thread reports readiness");
        Self {
            stop,
            join: Some(join),
        }
    }
}

impl Drop for ServerLoop {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

fn clock() -> ClockIdentity {
    ClockIdentity::new(
        ClockKind::Device,
        ClockSourceId::from_device_id([7; 16]).unwrap(),
        1,
    )
    .unwrap()
}

fn stamp(nanoseconds: i64) -> Timestamp {
    Timestamp::new(clock(), nanoseconds)
}

fn transform(x: f64) -> Transform3 {
    Transform3::new(
        Vec3::new(x, 0.0, 0.0).unwrap(),
        Quaternion::new(0.0, 0.0, 0.0, 1.0).unwrap(),
    )
}

fn set(revision: u64, x: f64) -> StaticSet {
    StaticSet {
        revision,
        effective_from: stamp(revision as i64),
        effective_uncertainty_ns: Some(0),
        transforms: vec![
            StaticEdge {
                parent: FrameId::new("world").unwrap(),
                child: FrameId::new("sensor").unwrap(),
                transform: transform(x),
            },
            StaticEdge {
                parent: FrameId::new("world").unwrap(),
                child: FrameId::new("camera").unwrap(),
                transform: transform(x + 1.0),
            },
        ],
    }
}

fn descriptors(
    update_authority: CapabilityInstanceId,
    snapshot_capability: CapabilityInstanceId,
    maximum_transforms: usize,
) -> (rbt_types::StreamDescriptor, RpcDescriptor) {
    let update = SliceStream::<StaticUpdate>::descriptor(
        update_authority.stream_service_name(),
        StreamProfile::Control,
    );
    let resources = RpcResourceConfig {
        max_response_slice_len: maximum_transforms,
        ..RpcResourceConfig::default()
    };
    let snapshot = RpcDescriptor::for_fixed_slice_with_resources::<
        StaticSnapshotRequest,
        StaticSnapshot,
    >(snapshot_capability.rpc_service_name(), resources);
    (update, snapshot)
}

fn buffer() -> TransformBuffer {
    TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap()
}

fn buffer_with_short_snapshot_age() -> TransformBuffer {
    let resources = TfResourceConfig {
        snapshot_max_age: Duration::from_millis(5),
        resync_interval: Duration::from_millis(5),
        ..TfResourceConfig::default()
    };
    TransformBuffer::new(resources, LookupPolicy::default()).unwrap()
}

#[test]
fn native_static_snapshot_late_join_update_and_explicit_oversize() {
    let update_authority = CapabilityInstanceId::new();
    let snapshot_capability = CapabilityInstanceId::new();
    let (update_descriptor, snapshot_descriptor) =
        descriptors(update_authority, snapshot_capability, 4);
    let provider = StaticTfProvider::open(
        &update_descriptor,
        &snapshot_descriptor,
        update_authority,
        snapshot_capability,
        set(10, 1.0),
        4,
    )
    .unwrap();
    let control = provider.control();
    let _server = ServerLoop::start(provider);
    let server_ready = Instant::now();
    while server_ready.elapsed() < Duration::from_secs(5) {
        thread::sleep(Duration::from_millis(10));
    }
    assert!(server_ready.elapsed() >= Duration::from_secs(5));

    let small_client = StaticTfClient::open(
        update_authority,
        &update_descriptor,
        &snapshot_descriptor,
        snapshot_capability,
        ActivationId::first(),
        "small",
        1,
        buffer(),
    )
    .unwrap();
    assert!(matches!(
        small_client.refresh_snapshot(Duration::from_secs(1)),
        Err(TransportError::Tf(TfError::SnapshotTooLarge))
    ));
    drop(small_client);

    let consumer_buffer = buffer();
    let client = StaticTfClient::open(
        update_authority,
        &update_descriptor,
        &snapshot_descriptor,
        snapshot_capability,
        ActivationId::new(2),
        "late",
        4,
        consumer_buffer.clone(),
    )
    .unwrap();
    client.refresh_snapshot(Duration::from_secs(1)).unwrap();
    let initial = consumer_buffer.pressure_snapshot().unwrap();
    assert_eq!(initial.providers[0].static_revision, Some(10));
    assert_eq!(initial.providers[0].active_static_edges, 2);
    let sensor_lookup = consumer_buffer
        .lookup_at_with(
            "world",
            "sensor",
            stamp(10),
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                ..LookupOptions::default()
            },
        )
        .unwrap();
    assert_eq!(sensor_lookup.transform.translation().x(), 1.0);
    let camera_lookup = consumer_buffer
        .lookup_at_with(
            "world",
            "camera",
            stamp(10),
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                ..LookupOptions::default()
            },
        )
        .unwrap();
    assert_eq!(camera_lookup.transform.translation().x(), 2.0);

    control.replace(set(11, 2.0)).unwrap();
    let drained = client.drain_updates(64).unwrap();
    assert_eq!(drained.accepted, 1);
    let after_update = consumer_buffer.pressure_snapshot().unwrap();
    assert_eq!(after_update.providers[0].static_revision, Some(11));
}

#[test]
fn native_dynamic_ingest_rejects_wrong_wire_authority_and_releases_guard() {
    let authority = CapabilityInstanceId::new();
    let wrong_authority = CapabilityInstanceId::new();
    let descriptor = Stream::<DynamicTransform>::descriptor(
        authority.stream_service_name(),
        StreamProfile::Control,
    );
    let publisher = Stream::<DynamicTransform>::publisher(&descriptor).unwrap();
    let subscriber =
        DynamicTfSubscriber::open(&descriptor, authority, "dynamic", buffer()).unwrap();
    let record = DynamicRecord {
        parent: FrameId::new("world").unwrap(),
        child: FrameId::new("tool").unwrap(),
        stamp: stamp(5),
        uncertainty_ns: Some(0),
        transform_epoch: 1,
        transform: transform(3.0),
    };

    let mut sample = publisher.loan().unwrap();
    *sample = wire::encode_dynamic(authority, &record);
    sample.send().unwrap();
    let valid = subscriber.drain_bounded(64).unwrap();
    assert_eq!(valid.accepted, 1);

    let mut sample = publisher.loan().unwrap();
    *sample = wire::encode_dynamic(wrong_authority, &record);
    sample.send().unwrap();
    let invalid = subscriber.drain_bounded(64).unwrap();
    assert_eq!(invalid.rejected, 1);
    assert!(matches!(
        invalid.failures.as_slice(),
        [TfError::ConflictingAuthority]
    ));
}

#[test]
fn native_static_snapshot_merges_an_update_received_while_rpc_is_in_flight() {
    let update_authority = CapabilityInstanceId::new();
    let snapshot_capability = CapabilityInstanceId::new();
    let (update_descriptor, snapshot_descriptor) =
        descriptors(update_authority, snapshot_capability, 4);
    let update_publisher = SliceStream::<StaticUpdate>::publisher(&update_descriptor, 4).unwrap();
    let mut snapshot_server = FixedSliceRpcServer::<StaticSnapshotRequest, StaticSnapshot>::
        open_or_create_for_capability(&snapshot_descriptor, snapshot_capability)
        .unwrap();
    let consumer_buffer = buffer();
    let client = Arc::new(
        StaticTfClient::open(
            update_authority,
            &update_descriptor,
            &snapshot_descriptor,
            snapshot_capability,
            ActivationId::new(3),
            "race",
            4,
            consumer_buffer.clone(),
        )
        .unwrap(),
    );

    let (request_tx, request_rx) = std::sync::mpsc::sync_channel(1);
    let (response_tx, response_rx) = std::sync::mpsc::sync_channel(1);
    let server = thread::spawn(move || {
        let request = snapshot_server
            .receive_timeout(Duration::from_secs(2))
            .unwrap()
            .unwrap();
        request_tx.send(()).unwrap();
        response_rx.recv_timeout(Duration::from_secs(2)).unwrap();

        let snapshot = set(10, 1.0);
        let mut response = request
            .loan_response_slice(snapshot.transforms.len())
            .unwrap();
        *response.user_header_mut() = wire::static_snapshot_header(update_authority, &snapshot);
        for (output, edge) in response.deref_mut().iter_mut().zip(&snapshot.transforms) {
            *output = wire::encode_static_edge(edge);
        }
        response.send().unwrap();
    });

    let refresh_client = Arc::clone(&client);
    let refresh = thread::spawn(move || refresh_client.refresh_snapshot(Duration::from_secs(2)));
    request_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    assert!(matches!(
        client.refresh_snapshot(Duration::from_secs(2)),
        Err(TransportError::SnapshotInFlight)
    ));

    let update = set(11, 2.0);
    let mut sample = update_publisher
        .loan_slice(update.transforms.len())
        .unwrap();
    *sample.header_mut() = wire::static_update_header(update_authority, &update);
    for (output, edge) in sample.payload_mut().iter_mut().zip(&update.transforms) {
        *output = wire::encode_static_edge(edge);
    }
    sample.send().unwrap();
    let staged = client.drain_updates(64).unwrap();
    assert_eq!(staged.accepted, 1);
    assert_eq!(
        consumer_buffer.pressure_snapshot().unwrap().providers[0].static_revision,
        None
    );

    response_tx.send(()).unwrap();
    refresh.join().unwrap().unwrap();
    server.join().unwrap();
    let after_snapshot = consumer_buffer.pressure_snapshot().unwrap();
    assert_eq!(after_snapshot.providers[0].static_revision, Some(11));
    assert!(!after_snapshot.providers[0].static_stale);
}

#[test]
fn malformed_update_during_snapshot_refresh_keeps_view_stale_until_authoritative_refresh() {
    let update_authority = CapabilityInstanceId::new();
    let snapshot_capability = CapabilityInstanceId::new();
    let (update_descriptor, snapshot_descriptor) =
        descriptors(update_authority, snapshot_capability, 4);
    let update_publisher = SliceStream::<StaticUpdate>::publisher(&update_descriptor, 4).unwrap();
    let mut snapshot_server = FixedSliceRpcServer::<StaticSnapshotRequest, StaticSnapshot>::
        open_or_create_for_capability(&snapshot_descriptor, snapshot_capability)
        .unwrap();
    let consumer_buffer = buffer();
    let client = Arc::new(
        StaticTfClient::open(
            update_authority,
            &update_descriptor,
            &snapshot_descriptor,
            snapshot_capability,
            ActivationId::new(6),
            "malformed_race",
            4,
            consumer_buffer.clone(),
        )
        .unwrap(),
    );

    let (request_tx, request_rx) = std::sync::mpsc::sync_channel(0);
    let (release_tx, release_rx) = std::sync::mpsc::sync_channel(0);
    let server = thread::spawn(move || {
        for call in 0..3 {
            let request = snapshot_server
                .receive_timeout(Duration::from_secs(2))
                .unwrap()
                .unwrap();
            let snapshot = if call == 2 {
                set(11, 2.0)
            } else {
                set(10, 1.0)
            };
            request_tx.send(()).unwrap();
            if call == 1 {
                release_rx.recv_timeout(Duration::from_secs(2)).unwrap();
            }

            let mut response = request
                .loan_response_slice(snapshot.transforms.len())
                .unwrap();
            *response.user_header_mut() = wire::static_snapshot_header(update_authority, &snapshot);
            for (output, edge) in response.deref_mut().iter_mut().zip(&snapshot.transforms) {
                *output = wire::encode_static_edge(edge);
            }
            response.send().unwrap();
        }
    });

    let initial_client = Arc::clone(&client);
    let initial_refresh =
        thread::spawn(move || initial_client.refresh_snapshot(Duration::from_secs(2)));
    request_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    initial_refresh.join().unwrap().unwrap();
    let healthy = consumer_buffer.pressure_snapshot().unwrap();
    assert_eq!(healthy.providers[0].static_revision, Some(10));
    assert!(!healthy.providers[0].static_stale);

    let refresh_client = Arc::clone(&client);
    let refresh = thread::spawn(move || refresh_client.refresh_snapshot(Duration::from_secs(2)));
    request_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    let malformed_update = set(11, 2.0);
    let mut malformed_edges: Vec<_> = malformed_update
        .transforms
        .iter()
        .map(wire::encode_static_edge)
        .collect();
    malformed_edges[0].transform.rotation = Default::default();
    let mut sample = update_publisher.loan_slice(malformed_edges.len()).unwrap();
    *sample.header_mut() = wire::static_update_header(update_authority, &malformed_update);
    sample.payload_mut().copy_from_slice(&malformed_edges);
    sample.send().unwrap();
    release_tx.send(()).unwrap();

    assert!(matches!(
        refresh.join().unwrap(),
        Err(TransportError::Tf(TfError::InvalidQuaternion))
    ));
    let stale = consumer_buffer.pressure_snapshot().unwrap();
    assert_eq!(stale.providers[0].static_revision, Some(10));
    assert!(stale.providers[0].static_stale);
    assert!(matches!(
        consumer_buffer.lookup_at_with(
            "world",
            "sensor",
            stamp(10),
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                ..LookupOptions::default()
            }
        ),
        Err(TfError::SnapshotStale)
    ));
    assert_eq!(
        client
            .update_pressure_snapshot()
            .subscriber
            .unwrap()
            .borrowed_samples,
        0
    );
    assert_eq!(
        client
            .snapshot_rpc_pressure()
            .client
            .unwrap()
            .response_guards_current,
        0
    );

    let recovery_client = Arc::clone(&client);
    let recovery = thread::spawn(move || recovery_client.refresh_snapshot(Duration::from_secs(2)));
    request_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    recovery.join().unwrap().unwrap();
    server.join().unwrap();
    let recovered = consumer_buffer.pressure_snapshot().unwrap();
    assert_eq!(recovered.providers[0].static_revision, Some(11));
    assert!(!recovered.providers[0].static_stale);

    assert!(matches!(
        client.refresh_snapshot(Duration::from_millis(50)),
        Err(TransportError::Rpc(_))
    ));
    let rpc_stale = consumer_buffer.pressure_snapshot().unwrap();
    assert_eq!(rpc_stale.providers[0].static_revision, Some(11));
    assert!(rpc_stale.providers[0].static_stale);
    assert_eq!(
        client
            .update_pressure_snapshot()
            .subscriber
            .unwrap()
            .borrowed_samples,
        0
    );
    let rpc_pressure = client.snapshot_rpc_pressure().client.unwrap();
    assert_eq!(rpc_pressure.response_guards_current, 0);
    assert_eq!(rpc_pressure.in_flight_current, 0);
}

#[test]
fn native_static_gap_is_resynchronized_from_the_provider_snapshot() {
    let update_authority = CapabilityInstanceId::new();
    let snapshot_capability = CapabilityInstanceId::new();
    let (update_descriptor, snapshot_descriptor) =
        descriptors(update_authority, snapshot_capability, 4);
    let provider = StaticTfProvider::open(
        &update_descriptor,
        &snapshot_descriptor,
        update_authority,
        snapshot_capability,
        set(20, 1.0),
        4,
    )
    .unwrap();
    let control = provider.control();
    let _server = ServerLoop::start(provider);
    let client_buffer = buffer();
    let client = StaticTfClient::open(
        update_authority,
        &update_descriptor,
        &snapshot_descriptor,
        snapshot_capability,
        ActivationId::new(4),
        "gap",
        4,
        client_buffer.clone(),
    )
    .unwrap();
    client.refresh_snapshot(Duration::from_secs(1)).unwrap();

    let gap_publisher = SliceStream::<StaticUpdate>::publisher(&update_descriptor, 4).unwrap();
    let gap = set(22, 3.0);
    let mut sample = gap_publisher.loan_slice(gap.transforms.len()).unwrap();
    *sample.header_mut() = wire::static_update_header(update_authority, &gap);
    for (output, edge) in sample.payload_mut().iter_mut().zip(&gap.transforms) {
        *output = wire::encode_static_edge(edge);
    }
    sample.send().unwrap();
    let gap_report = client.drain_updates(64).unwrap();
    assert_eq!(gap_report.rejected, 1);
    assert!(matches!(
        gap_report.failures.as_slice(),
        [TfError::SnapshotRevisionGap]
    ));
    assert!(client_buffer.pressure_snapshot().unwrap().providers[0].static_stale);

    control.replace(set(21, 2.0)).unwrap();
    control.replace(set(22, 3.0)).unwrap();
    let _ = client.drain_updates(64).unwrap();
    client.refresh_snapshot(Duration::from_secs(1)).unwrap();
    let recovered = client_buffer.pressure_snapshot().unwrap();
    assert_eq!(recovered.providers[0].static_revision, Some(22));
    assert!(!recovered.providers[0].static_stale);
}

#[test]
fn periodic_snapshot_refresh_recovers_when_the_final_update_is_not_drained() {
    let update_authority = CapabilityInstanceId::new();
    let snapshot_capability = CapabilityInstanceId::new();
    let (update_descriptor, snapshot_descriptor) =
        descriptors(update_authority, snapshot_capability, 4);
    let provider = StaticTfProvider::open(
        &update_descriptor,
        &snapshot_descriptor,
        update_authority,
        snapshot_capability,
        set(20, 1.0),
        4,
    )
    .unwrap();
    let control = provider.control();
    let _server = ServerLoop::start(provider);
    let client_buffer = buffer_with_short_snapshot_age();
    let client = StaticTfClient::open(
        update_authority,
        &update_descriptor,
        &snapshot_descriptor,
        snapshot_capability,
        ActivationId::new(5),
        "periodic",
        4,
        client_buffer.clone(),
    )
    .unwrap();
    client.refresh_snapshot(Duration::from_secs(1)).unwrap();
    control.replace(set(21, 2.0)).unwrap();

    // Model a missed final readiness notification: the update remains unread
    // while the periodic age check independently asks for the current set.
    thread::sleep(Duration::from_millis(12));
    let refreshed = client.refresh_if_due(Duration::from_secs(1)).unwrap();
    assert!(refreshed);
    assert_eq!(
        client_buffer.pressure_snapshot().unwrap().providers[0].static_revision,
        Some(21)
    );
}

#[test]
fn retained_static_control_cannot_keep_the_native_publisher_alive() {
    let update_authority = CapabilityInstanceId::new();
    let snapshot_capability = CapabilityInstanceId::new();
    let (update_descriptor, snapshot_descriptor) =
        descriptors(update_authority, snapshot_capability, 4);
    let provider = StaticTfProvider::open(
        &update_descriptor,
        &snapshot_descriptor,
        update_authority,
        snapshot_capability,
        set(10, 1.0),
        4,
    )
    .unwrap();
    let control = provider.control();
    assert!(
        local_pressure_snapshots()
            .iter()
            .any(|entry| entry.service_name == update_authority.stream_service_name())
    );

    drop(provider);

    assert!(matches!(
        control.replace(set(11, 2.0)),
        Err(TransportError::ActivationEnded)
    ));
    assert!(
        !local_pressure_snapshots()
            .iter()
            .any(|entry| entry.service_name == update_authority.stream_service_name())
    );
}

struct DynamicRole;

impl Capability for DynamicRole {
    const TYPE_NAME: &'static str = "test.dynamic.tf";
}

#[tokio::test]
async fn activation_scope_shutdown_closes_dynamic_handles_and_old_tokens() {
    let mut scope = EffectScope::new();
    let (publisher, capability) = {
        let mut context = ActivationCtx::new(Vec::new(), &mut scope);
        let provided = context
            .provide_dynamic_tf::<DynamicRole>(StreamProfile::Control)
            .unwrap();
        (provided.publisher, provided.capability)
    };
    let service_name = capability.stream_service_name();
    scope.shutdown().await;

    let record = DynamicRecord {
        parent: FrameId::new("world").unwrap(),
        child: FrameId::new("tool").unwrap(),
        stamp: stamp(5),
        uncertainty_ns: Some(0),
        transform_epoch: 1,
        transform: transform(3.0),
    };
    assert!(matches!(
        publisher.publish(&record),
        Err(TransportError::ActivationEnded)
    ));
    assert!(
        !local_pressure_snapshots()
            .iter()
            .any(|entry| entry.service_name == service_name)
    );

    let descriptor = Stream::<DynamicTransform>::descriptor(service_name, StreamProfile::Control);
    let _native_publisher = Stream::<DynamicTransform>::publisher(&descriptor).unwrap();
    let buffer = buffer();
    let provider_component = ComponentId::from("dynamic-provider");
    let binding = ResolvedBinding::new(
        "dynamic",
        DynamicRole::TYPE_NAME,
        provider_component.clone(),
        CapabilityDescriptor::new_stream(
            capability,
            provider_component,
            ProcessInstanceId::new(),
            ActivationId::first(),
            descriptor,
        ),
    );
    let mut consumer_scope = EffectScope::new();
    let old_token = {
        let mut context = ActivationCtx::new(vec![binding], &mut consumer_scope);
        context
            .require_dynamic_tf::<DynamicRole>("dynamic", "dynamic", buffer.clone())
            .unwrap()
            .authority
    };
    consumer_scope.shutdown().await;
    assert_eq!(buffer.pressure_snapshot().unwrap().active_providers, 0);
    assert!(matches!(
        buffer.ingest_dynamic(&old_token, record),
        Err(TfError::ProviderUnavailable)
    ));
}

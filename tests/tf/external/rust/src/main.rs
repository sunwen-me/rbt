use rbt::stream::Stream;
use rbt::tf::{
    DynamicRecord, DynamicTfPublisher, DynamicTfSubscriber, LookupOptions, LookupPolicy,
    Quaternion, StaticEdge, StaticSet, TfResourceConfig, Transform3, TransformBuffer, Vec3,
};
use rbt::time::DeviceClock;
use rbt::types::{CapabilityInstanceId, StreamProfile};
use rbt_types::DynamicTransform;

fn main() {
    let clock = DeviceClock::new([1; 16], 7).expect("device clock");
    let stamp = clock.stamp(42);
    let buffer = TransformBuffer::new(
        rbt::tf::TfResourceConfig::default(),
        rbt::tf::LookupPolicy::default(),
    )
    .expect("bounded TF buffer");
    let authority = buffer
        .bind_provider("external", rbt::types::CapabilityInstanceId::new())
        .expect("provider binds");
    buffer
        .apply_static_snapshot(
            &authority,
            StaticSet {
                revision: 1,
                effective_from: stamp,
                effective_uncertainty_ns: Some(0),
                transforms: vec![StaticEdge {
                    parent: "world".parse().expect("parent frame"),
                    child: "base".parse().expect("child frame"),
                    transform: Transform3::new(
                        Vec3::new(1.0, 2.0, 3.0).expect("translation"),
                        Quaternion::identity(),
                    ),
                }],
            },
        )
        .expect("snapshot applies");
    let result = buffer
        .lookup_at_with(
            "world",
            "base",
            stamp,
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                mappings: &[],
                now: None,
            },
        )
        .expect("local lookup");
    assert_eq!(result.transform.translation().x(), 1.0);
    assert_eq!(buffer.pressure_snapshot().unwrap().active_providers, 1);

    buffer
        .unbind_provider(&authority)
        .expect("provider unbinds");
    let after_unbind = buffer.pressure_snapshot().expect("empty snapshot");
    assert_eq!(after_unbind.active_providers, 0);
    assert!(after_unbind.graph.is_empty());

    let native_buffer = TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default())
        .expect("native transport buffer");
    let native_authority = CapabilityInstanceId::new();
    let native_descriptor = Stream::<DynamicTransform>::descriptor(
        native_authority.stream_service_name(),
        StreamProfile::Control,
    );
    let publisher = DynamicTfPublisher::open(&native_descriptor, native_authority)
        .expect("installed native TF publisher");
    let subscriber = DynamicTfSubscriber::open(
        &native_descriptor,
        native_authority,
        "external_native",
        native_buffer.clone(),
    )
    .expect("installed native TF subscriber");
    let native_record = DynamicRecord {
        parent: "native_world".parse().expect("parent frame"),
        child: "native_base".parse().expect("child frame"),
        stamp,
        uncertainty_ns: Some(0),
        transform_epoch: 1,
        transform: Transform3::new(
            Vec3::new(4.0, 5.0, 6.0).expect("translation"),
            Quaternion::identity(),
        ),
    };
    publisher
        .publish(&native_record)
        .expect("native TF sample publishes");
    let report = subscriber
        .drain_bounded(1)
        .expect("native TF sample drains");
    assert_eq!(
        (report.received, report.accepted, report.rejected),
        (1, 1, 0)
    );
    let native_result = native_buffer
        .lookup_at_with(
            "native_world",
            "native_base",
            stamp,
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                mappings: &[],
                now: None,
            },
        )
        .expect("native TF transport lookup");
    assert_eq!(native_result.transform.translation().x(), 4.0);
}

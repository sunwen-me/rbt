//! Cross-crate acceptance tests for the phase-two identity and recovery model.

use rbt_compose::{
    ActivationSnapshot, CapabilityRegistry, CapabilitySnapshot, ComponentSnapshot, ReconcilePlan,
    RequirementSnapshot, reconcile,
};
use rbt_core::{
    ActivationId, CapabilityDescriptor, CapabilityInstanceId, ComponentId, ComponentState,
    ProcessInstanceId, ProvidedCapability, RequirementSpec, ResolvedBinding, RpcDescriptor,
    StreamDescriptor,
};
use rbt_types::{Detection, FixedMessageAbi, Frame, StreamProfile};

struct Fixture {
    camera: ComponentSnapshot,
    detector: ComponentSnapshot,
    viewer: ComponentSnapshot,
    detector_process: ProcessInstanceId,
    detector_activation: ActivationId,
    detections_instance: CapabilityInstanceId,
}

fn descriptor<T: FixedMessageAbi>(
    component: &str,
    process: ProcessInstanceId,
    activation: ActivationId,
) -> CapabilityDescriptor {
    let instance = CapabilityInstanceId::new();
    CapabilityDescriptor::new(
        instance,
        ComponentId::from(component),
        process,
        activation,
        StreamDescriptor::for_fixed::<T>(instance.stream_service_name(), StreamProfile::Sensor),
    )
}

fn fixture() -> Fixture {
    let camera_process = ProcessInstanceId::new();
    let detector_process = ProcessInstanceId::new();
    let viewer_process = ProcessInstanceId::new();
    let camera_activation = ActivationId::first();
    let detector_activation = ActivationId::new(2);
    let viewer_activation = ActivationId::first();

    let camera_descriptor = descriptor::<Frame>("camera-a", camera_process, camera_activation);
    let camera_binding = ResolvedBinding::new(
        "camera",
        "Camera",
        ComponentId::from("camera-a"),
        camera_descriptor.clone(),
    );
    let detections_descriptor =
        descriptor::<Detection>("detector", detector_process, detector_activation);
    let detections_instance = detections_descriptor.capability_instance;
    let detections_binding = ResolvedBinding::new(
        "detections",
        "Detections",
        ComponentId::from("detector"),
        detections_descriptor.clone(),
    );

    let camera = ComponentSnapshot::new(
        "camera-a",
        camera_process,
        ComponentState::Active,
        Some(ActivationSnapshot::new(camera_activation, [])),
        std::iter::empty::<RequirementSnapshot>(),
        [CapabilitySnapshot::new(
            "Camera",
            camera_descriptor.capability_instance,
            camera_descriptor,
        )],
    );
    let detector = ComponentSnapshot::new(
        "detector",
        detector_process,
        ComponentState::Active,
        Some(ActivationSnapshot::new(
            detector_activation,
            [camera_binding.clone()],
        )),
        [RequirementSnapshot::new(
            RequirementSpec::new("camera", "Camera"),
            Some(camera_binding),
        )],
        [CapabilitySnapshot::new(
            "Detections",
            detections_instance,
            detections_descriptor,
        )],
    );
    let viewer = ComponentSnapshot::new(
        "viewer",
        viewer_process,
        ComponentState::Active,
        Some(ActivationSnapshot::new(
            viewer_activation,
            [detections_binding.clone()],
        )),
        [RequirementSnapshot::new(
            RequirementSpec::new("detections", "Detections"),
            Some(detections_binding),
        )],
        [],
    );

    Fixture {
        camera,
        detector,
        viewer,
        detector_process,
        detector_activation,
        detections_instance,
    }
}

#[test]
fn healthy_snapshots_reconstruct_reality_without_recreating_it() {
    // Invariant 4: composer restart reconstructs reality, not reality's
    // activation scopes or physical endpoints.
    let fixture = fixture();
    let mut registry = CapabilityRegistry::new();
    registry
        .rebuild_from_snapshots([&fixture.camera, &fixture.detector, &fixture.viewer])
        .unwrap();

    assert_eq!(
        reconcile(
            &registry,
            [&fixture.detector, &fixture.camera, &fixture.viewer]
        ),
        ReconcilePlan::default()
    );
    let detection = registry
        .resolve(&RequirementSpec::new("detections", "Detections"))
        .unwrap();
    assert_eq!(
        detection.descriptor.provider_process,
        fixture.detector_process
    );
    assert_eq!(detection.descriptor.activation, fixture.detector_activation);
    assert_eq!(
        detection.descriptor.capability_instance,
        fixture.detections_instance
    );
}

#[test]
fn composer_and_rpc_provider_outage_reconstructs_fresh_reality() {
    let provider = ComponentId::from("rpc-provider");
    let client = ComponentId::from("rpc-client");
    let provider_process_a = ProcessInstanceId::new();
    let client_process = ProcessInstanceId::new();
    let provider_activation_a = ActivationId::first();
    let client_activation_a = ActivationId::first();
    let instance_a = CapabilityInstanceId::new();
    let request = RequirementSpec::new("rpc", "RuntimeRpc");
    let descriptor_a = CapabilityDescriptor::new_rpc(
        instance_a,
        provider.clone(),
        provider_process_a,
        provider_activation_a,
        RpcDescriptor::for_fixed::<Frame, Detection>(instance_a.rpc_service_name()),
    );
    let provider_a = ComponentSnapshot::new(
        provider.clone(),
        provider_process_a,
        ComponentState::Active,
        Some(ActivationSnapshot::new(provider_activation_a, [])),
        std::iter::empty::<RequirementSnapshot>(),
        [CapabilitySnapshot::new(
            "RuntimeRpc",
            instance_a,
            descriptor_a.clone(),
        )],
    );
    let binding_a =
        ResolvedBinding::new("rpc", "RuntimeRpc", provider.clone(), descriptor_a.clone());
    let client_active_a = ComponentSnapshot::new(
        client.clone(),
        client_process,
        ComponentState::Active,
        Some(ActivationSnapshot::new(
            client_activation_a,
            [binding_a.clone()],
        )),
        [RequirementSnapshot::new(request.clone(), Some(binding_a))],
        std::iter::empty::<CapabilitySnapshot>(),
    );

    // A healthy Composer restart reconstructs the existing provider and
    // client identities; no lifecycle command is needed in either direction.
    let mut registry = CapabilityRegistry::new();
    registry
        .rebuild_from_snapshots([&provider_a, &client_active_a])
        .expect("healthy snapshots should reconstruct");
    assert_eq!(
        reconcile(&registry, [&provider_a, &client_active_a]),
        ReconcilePlan::default(),
        "healthy Composer restart must plan 0 deactivate / 0 activate"
    );
    assert_eq!(
        registry
            .resolve(&request)
            .expect("RPC provider should resolve")
            .descriptor
            .service_name(),
        instance_a.rpc_service_name()
    );

    // Losing Composer and provider together leaves a blocked, unbound client;
    // reconstruction must not recreate an endpoint or retain A's binding.
    registry.clear();
    let client_blocked = ComponentSnapshot::new(
        client.clone(),
        client_process,
        ComponentState::Blocked("waiting for RPC provider".into()),
        None,
        [RequirementSnapshot::unbound(request.clone())],
        std::iter::empty::<CapabilitySnapshot>(),
    );
    assert_eq!(
        reconcile(&registry, [&client_blocked]),
        ReconcilePlan::default(),
        "provider outage must leave the requirement BLOCKED"
    );

    let provider_process_b = ProcessInstanceId::new();
    let provider_activation_b = ActivationId::first();
    let instance_b = CapabilityInstanceId::new();
    assert_ne!(instance_a, instance_b);
    let descriptor_b = CapabilityDescriptor::new_rpc(
        instance_b,
        provider.clone(),
        provider_process_b,
        provider_activation_b,
        RpcDescriptor::for_fixed::<Frame, Detection>(instance_b.rpc_service_name()),
    );
    let provider_b = ComponentSnapshot::new(
        provider.clone(),
        provider_process_b,
        ComponentState::Active,
        Some(ActivationSnapshot::new(provider_activation_b, [])),
        std::iter::empty::<RequirementSnapshot>(),
        [CapabilitySnapshot::new(
            "RuntimeRpc",
            instance_b,
            descriptor_b.clone(),
        )],
    );
    registry
        .register_snapshot(&provider_b)
        .expect("replacement provider should reconstruct");
    assert_eq!(
        reconcile(&registry, [&client_blocked]).activate,
        vec![client.clone()]
    );

    let binding_b = ResolvedBinding::new("rpc", "RuntimeRpc", provider, descriptor_b.clone());
    let client_active_b = ComponentSnapshot::new(
        client.clone(),
        client_process,
        ComponentState::Active,
        Some(ActivationSnapshot::new(
            ActivationId::new(2),
            [binding_b.clone()],
        )),
        [RequirementSnapshot::new(request.clone(), Some(binding_b))],
        std::iter::empty::<CapabilitySnapshot>(),
    );
    assert_eq!(
        reconcile(&registry, [&provider_b, &client_active_b]),
        ReconcilePlan::default(),
        "reconstructed replacement should be healthy"
    );
    let resolved = registry
        .resolve(&request)
        .expect("replacement RPC provider should resolve");
    assert_eq!(resolved.descriptor.capability_instance, instance_b);
    assert_eq!(
        resolved.descriptor.service_name(),
        instance_b.rpc_service_name()
    );
    assert_ne!(
        resolved.descriptor.service_name(),
        instance_a.rpc_service_name()
    );
}

#[test]
fn activation_teardown_withdraws_only_its_owned_capability() {
    // Invariants 2 and 3: a capability cannot outlive the exact activation
    // that owns it, and a stale owner cannot delete its replacement.
    let fixture = fixture();
    let mut registry = CapabilityRegistry::new();
    registry
        .register_snapshot(&fixture.detector)
        .expect("valid detector snapshot");
    assert!(
        registry
            .resolve(&RequirementSpec::new("detections", "Detections"))
            .is_some()
    );

    assert!(!registry.remove_activation(
        &ComponentId::from("detector"),
        fixture.detector_process,
        ActivationId::first(),
    ));
    assert!(registry.remove_activation(
        &ComponentId::from("detector"),
        fixture.detector_process,
        fixture.detector_activation,
    ));
    assert!(
        registry
            .resolve(&RequirementSpec::new("detections", "Detections"))
            .is_none()
    );
}

#[test]
fn missing_provider_after_recovery_cascades_to_dependents() {
    let fixture = fixture();
    let mut registry = CapabilityRegistry::new();
    registry
        .rebuild_from_snapshots([&fixture.detector, &fixture.viewer])
        .unwrap();

    let first = reconcile(&registry, [&fixture.detector, &fixture.viewer]);
    assert_eq!(first.deactivate, vec![ComponentId::from("detector")]);

    registry.remove_activation(
        &ComponentId::from("detector"),
        fixture.detector_process,
        fixture.detector_activation,
    );
    let second = reconcile(&registry, [&fixture.viewer]);
    assert_eq!(second.deactivate, vec![ComponentId::from("viewer")]);
}

#[test]
fn snapshot_round_trip_preserves_physical_capability_identity() {
    let fixture = fixture();
    let encoded = serde_json::to_string(&fixture.detector).unwrap();
    let decoded: ComponentSnapshot = serde_json::from_str(&encoded).unwrap();
    let provided: Vec<ProvidedCapability> = decoded.provided().unwrap();

    assert_eq!(decoded.process_instance, fixture.detector_process);
    assert_eq!(
        decoded.activation.unwrap().activation_id,
        fixture.detector_activation
    );
    assert_eq!(
        provided[0].descriptor.capability_instance,
        fixture.detections_instance
    );
    assert_eq!(
        provided[0].descriptor.service_name(),
        fixture.detections_instance.stream_service_name()
    );
    assert_eq!(
        provided[0].descriptor.message_type().layout_hash,
        Detection::descriptor().layout_hash
    );
    assert_eq!(provided[0].descriptor.profile(), StreamProfile::Sensor);
}

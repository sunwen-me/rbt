//! Deterministic component lifecycle reconciliation.

use std::collections::BTreeMap;

use rbt_core::{ComponentId, ComponentState};
use serde::{Deserialize, Serialize};

use crate::{manifest::ComponentSnapshot, registry::CapabilityRegistry};

/// The component lifecycle operations required by one reconciliation pass.
#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct ReconcilePlan {
    /// Active components whose current bindings are no longer valid.
    pub deactivate: Vec<ComponentId>,
    /// Preparing or blocked components whose requirements are all available.
    pub activate: Vec<ComponentId>,
}

/// Computes stable lifecycle operations against a registry snapshot.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Reconciler {
    registry: CapabilityRegistry,
}

impl Reconciler {
    /// Creates a reconciler from an owned or borrowed registry snapshot.
    pub fn new(registry: impl Into<CapabilityRegistry>) -> Self {
        Self {
            registry: registry.into(),
        }
    }

    /// Returns the registry snapshot used by this reconciler.
    pub fn registry(&self) -> &CapabilityRegistry {
        &self.registry
    }

    /// Computes a deterministic plan for the supplied component snapshots.
    pub fn plan<I>(&self, snapshots: I) -> ReconcilePlan
    where
        I: IntoIterator,
        I::Item: SnapshotInput,
    {
        reconcile(&self.registry, snapshots)
    }

    /// Computes a plan against a registry supplied for one pass.
    pub fn plan_with_registry<I>(
        &self,
        registry: &CapabilityRegistry,
        snapshots: I,
    ) -> ReconcilePlan
    where
        I: IntoIterator,
        I::Item: SnapshotInput,
    {
        reconcile(registry, snapshots)
    }
}

/// Computes a deterministic reconciliation plan.
pub fn reconcile<I>(registry: &CapabilityRegistry, snapshots: I) -> ReconcilePlan
where
    I: IntoIterator,
    I::Item: SnapshotInput,
{
    let mut plan = ReconcilePlan::default();

    for snapshot in snapshots {
        let snapshot = snapshot.snapshot();
        match snapshot.state {
            ComponentState::Active => {
                if snapshot.activation.is_some() && active_bindings_are_stale(registry, snapshot) {
                    plan.deactivate.push(snapshot.id().clone());
                }
            }
            ComponentState::Blocked(_) | ComponentState::Preparing => {
                if snapshot
                    .requirements
                    .iter()
                    .all(|requirement| registry.resolve(&requirement.requirement).is_some())
                {
                    plan.activate.push(snapshot.id().clone());
                }
            }
            ComponentState::Declared
            | ComponentState::Activating
            | ComponentState::Quiescing
            | ComponentState::Failed(_) => {}
        }
    }

    plan.deactivate.sort();
    plan.deactivate.dedup();
    plan.activate.sort();
    plan.activate.dedup();
    plan
}

/// Input accepted by [`reconcile`]. Both owned snapshots and references to
/// snapshots are supported so a runtime can plan directly from its current
/// collection without cloning it.
pub trait SnapshotInput {
    /// Borrows the immutable snapshot used by the planner.
    fn snapshot(&self) -> &ComponentSnapshot;
}

impl SnapshotInput for ComponentSnapshot {
    fn snapshot(&self) -> &ComponentSnapshot {
        self
    }
}

impl SnapshotInput for &ComponentSnapshot {
    fn snapshot(&self) -> &ComponentSnapshot {
        self
    }
}

fn active_bindings_are_stale(registry: &CapabilityRegistry, snapshot: &ComponentSnapshot) -> bool {
    let Some(activation) = snapshot.activation.as_ref() else {
        return false;
    };
    if snapshot.requirements.iter().any(|requirement| {
        registry.resolve(&requirement.requirement).as_ref() != requirement.binding.as_ref()
    }) {
        return true;
    }

    // A binding not associated with one of the current requirements indicates
    // that the component's topology changed even if all expected bindings are
    // still present.
    if activation.bindings.iter().any(|binding| {
        !snapshot.requirements.iter().any(|requirement| {
            requirement.requirement.requirement_name == binding.requirement_name
                && requirement.requirement.capability_type == binding.capability_type
        })
    }) {
        return true;
    }

    // Duplicate bindings are malformed and should not be allowed to keep an
    // active component running with an ambiguous topology.
    let mut by_name = BTreeMap::new();
    activation.bindings.iter().any(|binding| {
        by_name
            .insert(binding.requirement_name.as_str(), binding)
            .is_some()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::{ActivationSnapshot, RequirementSnapshot};
    use rbt_core::{
        ActivationId, CapabilityDescriptor, CapabilityInstanceId, ProcessInstanceId,
        ProvidedCapability, RequirementSpec, StreamDescriptor,
    };
    use rbt_types::{Frame, StreamProfile};

    fn register(registry: &mut CapabilityRegistry, component: &str, capability_type: &str) {
        let component_id = ComponentId::from(component);
        let process = ProcessInstanceId::new();
        let activation = ActivationId::first();
        let instance = CapabilityInstanceId::new();
        let provided = ProvidedCapability::new(
            capability_type,
            CapabilityDescriptor::new(
                instance,
                component_id.clone(),
                process,
                activation,
                StreamDescriptor::for_fixed::<Frame>(
                    instance.stream_service_name(),
                    StreamProfile::Sensor,
                ),
            ),
        );
        registry
            .register_activation(component_id, process, activation, [provided])
            .unwrap();
    }

    fn requirement(name: &str, capability_type: &str) -> RequirementSpec {
        RequirementSpec::new(name, capability_type)
    }

    fn active_snapshot(
        id: &str,
        requirements: Vec<RequirementSpec>,
        bindings: Vec<rbt_core::ResolvedBinding>,
    ) -> ComponentSnapshot {
        let requirements = requirements
            .into_iter()
            .map(|requirement| {
                let binding = bindings
                    .iter()
                    .find(|binding| binding.requirement_name == requirement.requirement_name)
                    .cloned();
                RequirementSnapshot::new(requirement, binding)
            })
            .collect::<Vec<_>>();
        ComponentSnapshot::new(
            id,
            ProcessInstanceId::new(),
            ComponentState::Active,
            Some(ActivationSnapshot::new(ActivationId::first(), bindings)),
            requirements,
            [],
        )
    }

    #[test]
    fn blocked_and_preparing_activate_only_when_all_requirements_resolve() {
        let mut registry = CapabilityRegistry::new();
        register(&mut registry, "camera", "camera");
        let blocked = ComponentSnapshot::new(
            "consumer-b",
            ProcessInstanceId::new(),
            ComponentState::Blocked("waiting".into()),
            None,
            [RequirementSnapshot::unbound(requirement(
                "frames", "camera",
            ))],
            [],
        );
        let preparing = ComponentSnapshot::new(
            "consumer-a",
            ProcessInstanceId::new(),
            ComponentState::Preparing,
            None,
            [
                RequirementSnapshot::unbound(requirement("frames", "camera")),
                RequirementSnapshot::unbound(requirement("depth", "depth")),
            ],
            [],
        );

        let plan = reconcile(&registry, [blocked, preparing]);
        assert_eq!(plan.activate, vec![ComponentId::from("consumer-b")]);
        assert!(plan.deactivate.is_empty());
    }

    #[test]
    fn a_removed_camera_does_not_deactivate_an_unrelated_active_component() {
        let mut registry = CapabilityRegistry::new();
        register(&mut registry, "camera", "camera");
        register(&mut registry, "clock", "clock");

        let camera_binding = registry.resolve(&requirement("frames", "camera")).unwrap();
        let clock_binding = registry.resolve(&requirement("ticks", "clock")).unwrap();
        registry.remove_provider(&ComponentId::from("camera"));

        let camera_consumer = active_snapshot(
            "consumer-camera",
            vec![requirement("frames", "camera")],
            vec![camera_binding],
        );
        let clock_consumer = active_snapshot(
            "consumer-clock",
            vec![requirement("ticks", "clock")],
            vec![clock_binding],
        );

        let plan = reconcile(&registry, [clock_consumer, camera_consumer]);
        assert_eq!(plan.deactivate, vec![ComponentId::from("consumer-camera")]);
        assert!(plan.activate.is_empty());
    }

    #[test]
    fn active_replacement_deactivates_the_bound_component() {
        let mut registry = CapabilityRegistry::new();
        register(&mut registry, "camera", "camera");
        let old_binding = registry.resolve(&requirement("frames", "camera")).unwrap();
        register(&mut registry, "camera", "camera");

        let snapshot = active_snapshot(
            "consumer",
            vec![requirement("frames", "camera")],
            vec![old_binding],
        );
        let plan = reconcile(&registry, [snapshot]);
        assert_eq!(plan.deactivate, vec![ComponentId::from("consumer")]);
    }

    #[test]
    fn complete_healthy_reconstruction_requires_zero_lifecycle_operations() {
        let mut registry = CapabilityRegistry::new();
        register(&mut registry, "camera", "Camera");
        let camera_binding = registry.resolve(&requirement("camera", "Camera")).unwrap();
        register(&mut registry, "detector", "Detections");
        let detections_binding = registry
            .resolve(&requirement("detections", "Detections"))
            .unwrap();

        let camera = active_snapshot("camera", vec![], vec![]);
        let detector = active_snapshot(
            "detector",
            vec![requirement("camera", "Camera")],
            vec![camera_binding],
        );
        let viewer = active_snapshot(
            "viewer",
            vec![requirement("detections", "Detections")],
            vec![detections_binding],
        );

        assert_eq!(
            reconcile(&registry, [detector, camera, viewer]),
            ReconcilePlan::default()
        );
    }
}

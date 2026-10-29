//! Activation-owned capability registration and deterministic resolution.

use std::collections::BTreeMap;

use rbt_core::{
    ActivationId, CapabilityDescriptor, CapabilityInstanceId, ComponentId, ProcessInstanceId,
    ProvidedCapability, RequirementSpec, ResolvedBinding,
};
use thiserror::Error;

use crate::manifest::{ComponentSnapshot, SnapshotValidationError};

/// The provider information retained for one capability advertisement.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RegisteredCapability {
    /// Complete identity and physical endpoint of the capability instance.
    pub descriptor: CapabilityDescriptor,
}

impl RegisteredCapability {
    /// Creates a registered capability record.
    pub fn new(descriptor: CapabilityDescriptor) -> Self {
        Self { descriptor }
    }
}

/// A deterministic view of capability type -> provider component -> instance.
pub type RegistryTopology = BTreeMap<String, BTreeMap<ComponentId, RegisteredCapability>>;

/// A capability advertisement did not belong to the activation registering it.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
#[error(
    "capability {capability_instance} owner does not match component={component} process_instance={process} activation={activation}"
)]
pub struct CapabilityOwnerMismatch {
    /// Capability instance that carried the bad owner metadata.
    pub capability_instance: CapabilityInstanceId,
    /// Component expected to own the advertisement.
    pub component: ComponentId,
    /// Process expected to own the advertisement.
    pub process: ProcessInstanceId,
    /// Activation expected to own the advertisement.
    pub activation: ActivationId,
}

/// Registry of capabilities backed by currently valid activations.
///
/// Capability existence MUST be backed by a live valid Activation. Registration
/// validates the complete owner tuple, and teardown removes only that exact
/// tuple so a stale activation event cannot withdraw a replacement instance.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct CapabilityRegistry {
    topology: RegistryTopology,
}

impl CapabilityRegistry {
    /// Creates an empty registry.
    pub fn new() -> Self {
        Self::default()
    }

    /// Removes every registration. Used before reconstructing topology from
    /// a fresh set of component snapshots.
    pub fn clear(&mut self) {
        self.topology.clear();
    }

    /// Imports capabilities owned by one complete component snapshot.
    ///
    /// Only an `Active` snapshot with an activation can own registrations;
    /// all other lifecycle states are intentionally no-ops. Validation is
    /// performed before mutating this registry.
    pub fn register_snapshot(
        &mut self,
        snapshot: &ComponentSnapshot,
    ) -> Result<(), SnapshotValidationError> {
        snapshot.validate()?;
        let Some(activation) = snapshot.activation.as_ref() else {
            self.remove_provider(&snapshot.component_id);
            return Ok(());
        };
        if !matches!(snapshot.state, rbt_core::ComponentState::Active) {
            self.remove_provider(&snapshot.component_id);
            return Ok(());
        }
        let capabilities = snapshot.provided()?;
        self.register_activation(
            snapshot.component_id.clone(),
            snapshot.process_instance,
            activation.activation_id,
            capabilities,
        )
        .map_err(|_error| SnapshotValidationError::CapabilityOwnerMismatch)
    }

    /// Rebuilds all topology from a collection of complete snapshots.
    pub fn rebuild_from_snapshots<I, S>(
        &mut self,
        snapshots: I,
    ) -> Result<(), SnapshotValidationError>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<ComponentSnapshot>,
    {
        self.clear();
        for snapshot in snapshots {
            self.register_snapshot(snapshot.as_ref())?;
        }
        Ok(())
    }

    /// Imports one activation's complete provided-capability set.
    ///
    /// Validation happens before mutation. A malformed snapshot therefore
    /// cannot partially replace an existing, valid registration.
    pub fn register_activation<I>(
        &mut self,
        component: ComponentId,
        process: ProcessInstanceId,
        activation: ActivationId,
        capabilities: I,
    ) -> Result<(), CapabilityOwnerMismatch>
    where
        I: IntoIterator<Item = ProvidedCapability>,
    {
        let capabilities: Vec<_> = capabilities.into_iter().collect();
        for capability in &capabilities {
            let descriptor = &capability.descriptor;
            if descriptor.provider_component != component
                || descriptor.provider_process != process
                || descriptor.activation != activation
            {
                return Err(CapabilityOwnerMismatch {
                    capability_instance: descriptor.capability_instance,
                    component,
                    process,
                    activation,
                });
            }
        }

        self.remove_provider(&component);
        for capability in capabilities {
            self.topology
                .entry(capability.capability_type)
                .or_default()
                .insert(
                    component.clone(),
                    RegisteredCapability::new(capability.descriptor),
                );
        }
        Ok(())
    }

    /// Removes only capability registrations owned by the exact activation.
    pub fn remove_activation(
        &mut self,
        component: &ComponentId,
        process: ProcessInstanceId,
        activation: ActivationId,
    ) -> bool {
        self.remove_matching(component, |descriptor| {
            descriptor.provider_process == process && descriptor.activation == activation
        })
    }

    /// Removes registrations owned by an exact component process.
    pub fn remove_process(&mut self, component: &ComponentId, process: ProcessInstanceId) -> bool {
        self.remove_matching(component, |descriptor| {
            descriptor.provider_process == process
        })
    }

    /// Removes all registrations made by a logical component.
    pub fn remove_provider(&mut self, component: &ComponentId) -> bool {
        self.remove_matching(component, |_| true)
    }

    fn remove_matching(
        &mut self,
        component: &ComponentId,
        predicate: impl Fn(&CapabilityDescriptor) -> bool,
    ) -> bool {
        let capability_types: Vec<_> = self.topology.keys().cloned().collect();
        let mut removed = false;
        for capability_type in capability_types {
            if let Some(providers) = self.topology.get_mut(&capability_type) {
                if providers
                    .get(component)
                    .is_some_and(|record| predicate(&record.descriptor))
                {
                    providers.remove(component);
                    removed = true;
                }
                if providers.is_empty() {
                    self.topology.remove(&capability_type);
                }
            }
        }
        removed
    }

    /// Returns a stable clone of the current topology.
    pub fn topology(&self) -> RegistryTopology {
        self.topology.clone()
    }

    /// Returns providers in lexicographic component order.
    pub fn providers(
        &self,
        capability_type: &str,
    ) -> Option<&BTreeMap<ComponentId, RegisteredCapability>> {
        self.topology.get(capability_type)
    }

    /// Returns the current advertisements for one component.
    pub fn capabilities_for(&self, component: &ComponentId) -> Vec<ProvidedCapability> {
        self.topology
            .iter()
            .filter_map(|(capability_type, providers)| {
                providers.get(component).map(|record| {
                    ProvidedCapability::new(capability_type.clone(), record.descriptor.clone())
                })
            })
            .collect()
    }

    /// Resolves to the lexicographically smallest provider component.
    pub fn resolve(&self, requirement: &RequirementSpec) -> Option<ResolvedBinding> {
        let (provider, record) = self
            .topology
            .get(&requirement.capability_type)?
            .first_key_value()?;
        Some(ResolvedBinding::new(
            requirement.requirement_name.clone(),
            requirement.capability_type.clone(),
            provider.clone(),
            record.descriptor.clone(),
        ))
    }
}

impl From<&CapabilityRegistry> for CapabilityRegistry {
    fn from(registry: &CapabilityRegistry) -> Self {
        registry.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rbt_core::{RpcDescriptor, StreamDescriptor};
    use rbt_types::{Detection, Frame, StreamProfile};

    fn provided(
        component: &str,
        process: ProcessInstanceId,
        activation: ActivationId,
        capability_type: &str,
    ) -> ProvidedCapability {
        let instance = CapabilityInstanceId::new();
        ProvidedCapability::new(
            capability_type,
            CapabilityDescriptor::new(
                instance,
                ComponentId::from(component),
                process,
                activation,
                StreamDescriptor::for_fixed::<Frame>(
                    instance.stream_service_name(),
                    StreamProfile::Sensor,
                ),
            ),
        )
    }

    fn requirement(capability_type: &str) -> RequirementSpec {
        RequirementSpec::new("input", capability_type)
    }

    fn provided_rpc(
        component: &str,
        process: ProcessInstanceId,
        activation: ActivationId,
        capability_type: &str,
    ) -> ProvidedCapability {
        let instance = CapabilityInstanceId::new();
        ProvidedCapability::new(
            capability_type,
            CapabilityDescriptor::new_rpc(
                instance,
                ComponentId::from(component),
                process,
                activation,
                RpcDescriptor::for_fixed::<Detection, Frame>(instance.rpc_service_name()),
            ),
        )
    }

    #[test]
    fn registration_resolves_and_activation_teardown_removes_capability() {
        let mut registry = CapabilityRegistry::new();
        let component = ComponentId::from("camera");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::first();
        let capability = provided("camera", process, activation, "Camera");
        let instance = capability.descriptor.capability_instance;

        registry
            .register_activation(component.clone(), process, activation, [capability])
            .unwrap();
        assert_eq!(
            registry
                .resolve(&requirement("Camera"))
                .unwrap()
                .descriptor
                .capability_instance,
            instance
        );
        assert!(registry.remove_activation(&component, process, activation));
        assert!(registry.resolve(&requirement("Camera")).is_none());
    }

    #[test]
    fn stale_activation_teardown_cannot_remove_replacement_instance() {
        let mut registry = CapabilityRegistry::new();
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let first = ActivationId::first();
        let second = first.next();
        let old = provided("detector", process, first, "Detections");
        let new = provided("detector", process, second, "Detections");
        let old_instance = old.descriptor.capability_instance;
        let new_instance = new.descriptor.capability_instance;
        assert_ne!(old_instance, new_instance);

        registry
            .register_activation(component.clone(), process, first, [old])
            .unwrap();
        registry
            .register_activation(component.clone(), process, second, [new])
            .unwrap();
        assert!(!registry.remove_activation(&component, process, first));
        assert_eq!(
            registry
                .resolve(&requirement("Detections"))
                .unwrap()
                .descriptor
                .capability_instance,
            new_instance
        );
    }

    #[test]
    fn owner_mismatch_is_rejected_without_mutating_registry() {
        let mut registry = CapabilityRegistry::new();
        let process = ProcessInstanceId::new();
        let activation = ActivationId::first();
        let malformed = provided("other", process, activation, "Camera");
        let error = registry
            .register_activation(
                ComponentId::from("camera"),
                process,
                activation,
                [malformed],
            )
            .unwrap_err();
        assert_eq!(error.component, ComponentId::from("camera"));
        assert!(registry.topology().is_empty());
    }

    #[test]
    fn provider_selection_is_stable_and_lexicographic() {
        let mut registry = CapabilityRegistry::new();
        let process_z = ProcessInstanceId::new();
        let process_a = ProcessInstanceId::new();
        registry
            .register_activation(
                ComponentId::from("z-camera"),
                process_z,
                ActivationId::first(),
                [provided(
                    "z-camera",
                    process_z,
                    ActivationId::first(),
                    "Camera",
                )],
            )
            .unwrap();
        registry
            .register_activation(
                ComponentId::from("a-camera"),
                process_a,
                ActivationId::first(),
                [provided(
                    "a-camera",
                    process_a,
                    ActivationId::first(),
                    "Camera",
                )],
            )
            .unwrap();

        assert_eq!(
            registry.resolve(&requirement("Camera")).unwrap().provider,
            ComponentId::from("a-camera")
        );
    }

    #[test]
    fn rpc_provider_resolves_and_replacement_rotates_physical_endpoint() {
        let mut registry = CapabilityRegistry::new();
        let component = ComponentId::from("rpc-provider");
        let process = ProcessInstanceId::new();
        let first = ActivationId::first();
        let second = first.next();
        let old = provided_rpc("rpc-provider", process, first, "DetectorRpc");
        let new = provided_rpc("rpc-provider", process, second, "DetectorRpc");
        let old_endpoint = old.descriptor.service_name().to_owned();
        let new_endpoint = new.descriptor.service_name().to_owned();
        assert_ne!(old_endpoint, new_endpoint);
        assert_eq!(
            old.descriptor.transport_kind(),
            rbt_core::CapabilityTransportKind::Rpc
        );

        registry
            .register_activation(component.clone(), process, first, [old])
            .unwrap();
        assert_eq!(
            registry
                .resolve(&requirement("DetectorRpc"))
                .unwrap()
                .descriptor
                .service_name(),
            old_endpoint
        );
        registry
            .register_activation(component.clone(), process, second, [new])
            .unwrap();
        assert!(!registry.remove_activation(&component, process, first));
        assert_eq!(
            registry
                .resolve(&requirement("DetectorRpc"))
                .unwrap()
                .descriptor
                .service_name(),
            new_endpoint
        );
    }
}

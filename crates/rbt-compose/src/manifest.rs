//! Component manifests and immutable control-plane snapshots.
//!
//! A snapshot is the component's local truth. In particular, an activation
//! owns both the bindings it consumed and the capabilities it published. A
//! composer may reconstruct its in-memory registry from snapshots, but it
//! must never infer a live capability from a component that is not `Active`.

use std::collections::BTreeMap;

use rbt_core::{
    ActivationId, CapabilityDescriptor, ComponentId, ComponentState, ProcessInstanceId,
    ProvidedCapability, RequirementSpec, ResolvedBinding, RpcPressure,
};
use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::protocol::COMPOSER_SNAPSHOT_VERSION;

/// The static part of a component declaration.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ComponentManifest {
    /// Stable component identity.
    pub id: ComponentId,
    /// Capabilities advertised by this component.
    pub provides: Vec<ProvidedCapability>,
    /// Capabilities required by this component.
    pub requirements: Vec<RequirementSpec>,
}

impl ComponentManifest {
    /// Creates a manifest from the component's declarations.
    pub fn new(
        id: impl Into<ComponentId>,
        provides: impl IntoIterator<Item = ProvidedCapability>,
        requirements: impl IntoIterator<Item = RequirementSpec>,
    ) -> Self {
        Self {
            id: id.into(),
            provides: provides.into_iter().collect(),
            requirements: requirements.into_iter().collect(),
        }
    }

    /// Creates a manifest containing no providers or requirements.
    pub fn empty(id: impl Into<ComponentId>) -> Self {
        Self::new(id, [], [])
    }
}

/// One declared requirement and the binding currently installed for it.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct RequirementSnapshot {
    /// The stable declaration made by the component.
    pub requirement: RequirementSpec,
    /// The resolver-selected binding, if this activation has one.
    pub binding: Option<ResolvedBinding>,
}

impl RequirementSnapshot {
    /// Creates a requirement snapshot.
    pub fn new(requirement: RequirementSpec, binding: Option<ResolvedBinding>) -> Self {
        Self {
            requirement,
            binding,
        }
    }

    /// Creates a requirement snapshot without an installed binding.
    pub fn unbound(requirement: RequirementSpec) -> Self {
        Self::new(requirement, None)
    }

    /// Validates that an optional binding identifies this same requirement.
    pub fn validate(&self) -> Result<(), SnapshotValidationError> {
        if let Some(binding) = &self.binding {
            if binding.requirement_name != self.requirement.requirement_name {
                return Err(SnapshotValidationError::RequirementBindingName {
                    requirement: self.requirement.requirement_name.clone(),
                    binding: binding.requirement_name.clone(),
                });
            }
            if binding.capability_type != self.requirement.capability_type {
                return Err(SnapshotValidationError::RequirementBindingType {
                    requirement: self.requirement.capability_type.clone(),
                    binding: binding.capability_type.clone(),
                });
            }
        }
        Ok(())
    }
}

impl From<RequirementSpec> for RequirementSnapshot {
    fn from(requirement: RequirementSpec) -> Self {
        Self::unbound(requirement)
    }
}

/// The activation-owned part of a component snapshot.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ActivationSnapshot {
    /// Monotonic identity assigned by the component process.
    pub activation_id: ActivationId,
    /// Bindings consumed by this activation.
    pub bindings: Vec<ResolvedBinding>,
}

impl ActivationSnapshot {
    /// Creates an activation snapshot.
    pub fn new(
        activation_id: ActivationId,
        bindings: impl IntoIterator<Item = ResolvedBinding>,
    ) -> Self {
        Self {
            activation_id,
            bindings: bindings.into_iter().collect(),
        }
    }

    /// Returns the binding selected for a requirement, if present.
    pub fn binding_for(&self, requirement_name: &str) -> Option<&ResolvedBinding> {
        self.bindings
            .iter()
            .find(|binding| binding.requirement_name == requirement_name)
    }

    /// Returns the bindings indexed by requirement name.
    pub fn binding_map(&self) -> BTreeMap<&str, &ResolvedBinding> {
        self.bindings
            .iter()
            .map(|binding| (binding.requirement_name.as_str(), binding))
            .collect()
    }

    /// Validates duplicate requirement names in this activation.
    pub fn validate(&self) -> Result<(), SnapshotValidationError> {
        let mut names = BTreeMap::new();
        for binding in &self.bindings {
            if names
                .insert(binding.requirement_name.as_str(), ())
                .is_some()
            {
                return Err(SnapshotValidationError::DuplicateBinding(
                    binding.requirement_name.clone(),
                ));
            }
        }
        Ok(())
    }
}

/// One activation-owned capability advertisement.
///
/// `capability_instance` is repeated outside `descriptor` deliberately: it
/// makes the snapshot's identity easy to inspect and validates against the
/// complete descriptor during import.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct CapabilitySnapshot {
    /// Stable capability role (for example, `Camera`).
    pub capability_type: String,
    /// Identity of the advertised capability instance.
    pub capability_instance: rbt_core::CapabilityInstanceId,
    /// Complete owner and data-plane endpoint metadata.
    pub descriptor: CapabilityDescriptor,
}

impl CapabilitySnapshot {
    /// Creates a capability snapshot from its wire fields.
    pub fn new(
        capability_type: impl Into<String>,
        capability_instance: rbt_core::CapabilityInstanceId,
        descriptor: CapabilityDescriptor,
    ) -> Self {
        Self {
            capability_type: capability_type.into(),
            capability_instance,
            descriptor,
        }
    }

    /// Converts an advertised capability into snapshot form.
    pub fn from_provided(capability: &ProvidedCapability) -> Self {
        Self::new(
            capability.capability_type.clone(),
            capability.descriptor.capability_instance,
            capability.descriptor.clone(),
        )
    }

    /// Converts an advertised capability into snapshot form.
    pub fn from(capability: &ProvidedCapability) -> Self {
        Self::from_provided(capability)
    }

    /// Converts this snapshot into the registry's untyped advertisement.
    pub fn to_provided(&self) -> Result<ProvidedCapability, SnapshotValidationError> {
        self.validate()?;
        Ok(ProvidedCapability::new(
            self.capability_type.clone(),
            self.descriptor.clone(),
        ))
    }

    /// Consumes this snapshot and converts it into an advertisement.
    pub fn into_provided(self) -> Result<ProvidedCapability, SnapshotValidationError> {
        self.validate()?;
        Ok(ProvidedCapability::new(
            self.capability_type,
            self.descriptor,
        ))
    }

    /// Validates the duplicated capability-instance identity.
    pub fn validate(&self) -> Result<(), SnapshotValidationError> {
        if self.capability_instance != self.descriptor.capability_instance {
            return Err(SnapshotValidationError::CapabilityInstanceMismatch {
                outer: self.capability_instance,
                descriptor: self.descriptor.capability_instance,
            });
        }
        if self.capability_type.is_empty() {
            return Err(SnapshotValidationError::EmptyCapabilityType);
        }
        Ok(())
    }
}

impl From<&ProvidedCapability> for CapabilitySnapshot {
    fn from(capability: &ProvidedCapability) -> Self {
        Self::from_provided(capability)
    }
}

/// A complete point-in-time lifecycle and topology view of one component.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ComponentSnapshot {
    /// Version of this component snapshot representation.
    pub snapshot_version: u16,
    /// Stable component identity.
    pub component_id: ComponentId,
    /// Identity of the process lifetime publishing this snapshot.
    pub process_instance: ProcessInstanceId,
    /// Current lifecycle state in the component process.
    pub state: ComponentState,
    /// Current activation, including all activation-owned bindings.
    pub activation: Option<ActivationSnapshot>,
    /// Declared requirements and their currently installed bindings.
    pub requirements: Vec<RequirementSnapshot>,
    /// Capabilities published by the current activation.
    pub provided_capabilities: Vec<CapabilitySnapshot>,
    /// Low-rate, process-local stream pressure observations.
    #[serde(default)]
    pub stream_pressures: Vec<rbt_core::StreamPressure>,
    /// Low-rate, process-local RPC pressure observations.
    #[serde(default)]
    pub rpc_pressures: Vec<RpcPressure>,
}

impl ComponentSnapshot {
    /// Creates a complete component snapshot. `requirements` accepts either
    /// `RequirementSnapshot` values or bare `RequirementSpec` declarations.
    pub fn new<R>(
        component_id: impl Into<ComponentId>,
        process_instance: ProcessInstanceId,
        state: ComponentState,
        activation: Option<ActivationSnapshot>,
        requirements: impl IntoIterator<Item = R>,
        provided_capabilities: impl IntoIterator<Item = CapabilitySnapshot>,
    ) -> Self
    where
        R: Into<RequirementSnapshot>,
    {
        Self {
            snapshot_version: COMPOSER_SNAPSHOT_VERSION,
            component_id: component_id.into(),
            process_instance,
            state,
            activation,
            requirements: requirements.into_iter().map(Into::into).collect(),
            provided_capabilities: provided_capabilities.into_iter().collect(),
            stream_pressures: Vec::new(),
            rpc_pressures: Vec::new(),
        }
    }

    /// Attaches process-local pressure observations to this snapshot.
    pub fn with_stream_pressures(
        mut self,
        stream_pressures: impl IntoIterator<Item = rbt_core::StreamPressure>,
    ) -> Self {
        self.stream_pressures = stream_pressures.into_iter().collect();
        self
    }

    /// Attaches process-local RPC pressure observations to this snapshot.
    pub fn with_rpc_pressures(
        mut self,
        rpc_pressures: impl IntoIterator<Item = RpcPressure>,
    ) -> Self {
        self.rpc_pressures = rpc_pressures.into_iter().collect();
        self
    }

    /// Returns the process-local RPC pressure observations carried by this
    /// snapshot.
    pub fn rpc_pressures(&self) -> &[RpcPressure] {
        &self.rpc_pressures
    }

    /// Creates a complete snapshot from explicit detail vectors.
    pub fn with_details(
        component_id: impl Into<ComponentId>,
        process_instance: ProcessInstanceId,
        state: ComponentState,
        activation: Option<ActivationSnapshot>,
        requirements: impl IntoIterator<Item = RequirementSnapshot>,
        provided_capabilities: impl IntoIterator<Item = CapabilitySnapshot>,
    ) -> Self {
        Self::new(
            component_id,
            process_instance,
            state,
            activation,
            requirements,
            provided_capabilities,
        )
    }

    /// Creates a snapshot from a component manifest with no activation.
    pub fn from_manifest(
        manifest: &ComponentManifest,
        process_instance: ProcessInstanceId,
        state: ComponentState,
    ) -> Self {
        Self::new(
            manifest.id.clone(),
            process_instance,
            state,
            None,
            manifest
                .requirements
                .iter()
                .cloned()
                .map(RequirementSnapshot::unbound),
            [],
        )
    }

    /// Creates a local placeholder used before the first component snapshot.
    pub fn placeholder(component_id: ComponentId, process_instance: ProcessInstanceId) -> Self {
        Self::new(
            component_id,
            process_instance,
            ComponentState::Preparing,
            None,
            std::iter::empty::<RequirementSnapshot>(),
            std::iter::empty::<CapabilitySnapshot>(),
        )
    }

    /// Validates identity, requirement bindings, and activation ownership.
    pub fn validate(&self) -> Result<(), SnapshotValidationError> {
        if self.snapshot_version != COMPOSER_SNAPSHOT_VERSION {
            return Err(SnapshotValidationError::SnapshotVersionMismatch {
                expected: COMPOSER_SNAPSHOT_VERSION,
                actual: self.snapshot_version,
            });
        }
        if matches!(self.state, ComponentState::Active) && self.activation.is_none() {
            return Err(SnapshotValidationError::ActiveWithoutActivation);
        }
        let mut requirement_names = BTreeMap::new();
        for requirement in &self.requirements {
            requirement.validate()?;
            if requirement_names
                .insert(requirement.requirement.requirement_name.as_str(), ())
                .is_some()
            {
                return Err(SnapshotValidationError::DuplicateRequirement(
                    requirement.requirement.requirement_name.clone(),
                ));
            }
        }

        if let Some(activation) = &self.activation {
            activation.validate()?;
            for binding in &activation.bindings {
                let Some(requirement) = self.requirements.iter().find(|requirement| {
                    requirement.requirement.requirement_name == binding.requirement_name
                }) else {
                    return Err(SnapshotValidationError::UnknownBinding(
                        binding.requirement_name.clone(),
                    ));
                };
                if binding.capability_type != requirement.requirement.capability_type {
                    return Err(SnapshotValidationError::RequirementBindingType {
                        requirement: requirement.requirement.capability_type.clone(),
                        binding: binding.capability_type.clone(),
                    });
                }
                if requirement.binding.as_ref() != Some(binding) {
                    return Err(SnapshotValidationError::ActivationBindingMismatch(
                        binding.requirement_name.clone(),
                    ));
                }
            }
        } else if self
            .requirements
            .iter()
            .any(|requirement| requirement.binding.is_some())
        {
            return Err(SnapshotValidationError::BoundRequirementWithoutActivation);
        }

        for capability in &self.provided_capabilities {
            capability.validate()?;
            let descriptor = &capability.descriptor;
            if descriptor.provider_component != self.component_id
                || descriptor.provider_process != self.process_instance
                || self
                    .activation
                    .as_ref()
                    .is_none_or(|activation| descriptor.activation != activation.activation_id)
            {
                return Err(SnapshotValidationError::CapabilityOwnerMismatch);
            }
        }

        // Non-active snapshots may retain activation-owned metadata while
        // draining. Registry import applies the stricter rule and only
        // registers capabilities for `Active` snapshots.
        Ok(())
    }

    /// Stable component identity (alias retained for older callers).
    pub fn id(&self) -> &ComponentId {
        &self.component_id
    }

    /// Returns the currently installed binding for a requirement.
    pub fn binding_for(&self, requirement_name: &str) -> Option<&ResolvedBinding> {
        self.requirements
            .iter()
            .find(|requirement| requirement.requirement.requirement_name == requirement_name)
            .and_then(|requirement| requirement.binding.as_ref())
    }

    /// Returns activation-owned bindings, if this snapshot has an activation.
    pub fn bindings(&self) -> &[ResolvedBinding] {
        self.activation
            .as_ref()
            .map_or(&[], |activation| activation.bindings.as_slice())
    }

    /// Returns the declared requirement specifications.
    pub fn requirement_specs(&self) -> impl Iterator<Item = &RequirementSpec> {
        self.requirements
            .iter()
            .map(|requirement| &requirement.requirement)
    }

    /// Converts all capability snapshots into registry advertisements.
    pub fn provided(&self) -> Result<Vec<ProvidedCapability>, SnapshotValidationError> {
        self.validate()?;
        self.provided_capabilities
            .iter()
            .map(CapabilitySnapshot::to_provided)
            .collect()
    }
}

impl AsRef<ComponentSnapshot> for ComponentSnapshot {
    fn as_ref(&self) -> &ComponentSnapshot {
        self
    }
}

/// Validation failures for untrusted component snapshots.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
pub enum SnapshotValidationError {
    /// The snapshot representation is not understood by this composer.
    #[error(
        "component snapshot version {actual} does not match composer snapshot version {expected}"
    )]
    SnapshotVersionMismatch {
        /// Version required by this composer.
        expected: u16,
        /// Version carried by the untrusted snapshot.
        actual: u16,
    },
    /// An active component must identify the activation backing its reality.
    #[error("Active snapshot has no activation")]
    ActiveWithoutActivation,
    /// A requirement has more than one entry.
    #[error("duplicate requirement {0:?}")]
    DuplicateRequirement(String),
    /// An activation has more than one binding for a requirement.
    #[error("duplicate binding for requirement {0:?}")]
    DuplicateBinding(String),
    /// A binding names a requirement that was not declared.
    #[error("binding names unknown requirement {0:?}")]
    UnknownBinding(String),
    /// A binding's requirement name does not match its declaration.
    #[error("binding requirement name {binding:?} does not match {requirement:?}")]
    RequirementBindingName {
        requirement: String,
        binding: String,
    },
    /// A binding's capability type does not match its declaration.
    #[error("binding capability type {binding:?} does not match {requirement:?}")]
    RequirementBindingType {
        requirement: String,
        binding: String,
    },
    /// Requirement-level and activation-level binding metadata disagree.
    #[error("activation binding for requirement {0:?} disagrees with requirement snapshot")]
    ActivationBindingMismatch(String),
    /// A requirement binding exists without an owning activation.
    #[error("bound requirement exists without an activation")]
    BoundRequirementWithoutActivation,
    /// Capability identity was duplicated inconsistently.
    #[error("capability instance differs between snapshot and descriptor")]
    CapabilityInstanceMismatch {
        /// Identity stored on the snapshot.
        outer: rbt_core::CapabilityInstanceId,
        /// Identity stored by the descriptor.
        descriptor: rbt_core::CapabilityInstanceId,
    },
    /// A capability type cannot be empty.
    #[error("capability type is empty")]
    EmptyCapabilityType,
    /// A capability descriptor's owner does not match this component.
    #[error("capability descriptor owner does not match component snapshot")]
    CapabilityOwnerMismatch,
    /// Only active snapshots may publish capabilities.
    #[error("capabilities may only be published by an Active snapshot")]
    CapabilitiesOutsideActive,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn snapshot_deserialization_defaults_missing_rpc_pressures() {
        let snapshot = ComponentSnapshot::new(
            "component",
            ProcessInstanceId::new(),
            ComponentState::Preparing,
            None,
            std::iter::empty::<RequirementSpec>(),
            std::iter::empty::<CapabilitySnapshot>(),
        );
        let mut value = serde_json::to_value(&snapshot).unwrap();
        value
            .as_object_mut()
            .expect("snapshot serializes as an object")
            .remove("rpc_pressures");

        let restored: ComponentSnapshot = serde_json::from_value(value).unwrap();
        assert!(restored.rpc_pressures.is_empty());
    }
}

//! NDJSON messages for the low-rate composition control plane.
//!
//! The protocol deliberately carries topology metadata only. Stream payloads
//! stay on the data plane; this crate never serializes or forwards samples.

use rbt_core::{
    ActivationId, ComponentId, ComposerEpoch, ProcessInstanceId, ResolvedBinding, Revision,
};
use serde::{Deserialize, Serialize};

use crate::manifest::ComponentSnapshot;

/// Current low-rate control-plane protocol version.
pub const CONTROL_PROTOCOL_VERSION: u16 = 2;

/// Current component-snapshot representation version.
pub const COMPOSER_SNAPSHOT_VERSION: u16 = 4;

/// Backwards-compatible name for [`CONTROL_PROTOCOL_VERSION`].
#[deprecated(note = "use CONTROL_PROTOCOL_VERSION instead")]
pub const PROTOCOL_VERSION: u16 = CONTROL_PROTOCOL_VERSION;

/// The way a supervised activation task stopped.
///
/// This remains a runtime diagnostic type even though task-specific events are
/// no longer individual wire messages. The reason is included in a failed
/// component snapshot instead.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum TaskOutcome {
    /// The task returned successfully.
    Completed,
    /// The task returned an error.
    Error(String),
    /// The task panicked while being polled.
    Panicked,
}

/// A message sent by a component process to the composer.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ClientMessage {
    /// Declares the process identity and protocol version.
    Hello {
        /// Stable component identity.
        component_id: ComponentId,
        /// Identity of this process lifetime.
        process_instance: ProcessInstanceId,
        /// Version of the control protocol spoken by this process.
        control_protocol_version: u16,
        /// Version of the component snapshot representation spoken by this process.
        snapshot_version: u16,
    },
    /// Publishes the component's complete local truth.
    Snapshot {
        /// Point-in-time lifecycle, binding, and capability view.
        snapshot: ComponentSnapshot,
    },
    /// Requests a read-only diagnostic view from the composer.
    Inspect {
        /// Version of the control protocol spoken by the requester.
        control_protocol_version: u16,
        /// Version of the snapshot representation understood by the requester.
        snapshot_version: u16,
    },
}

impl ClientMessage {
    /// Constructs a current-version Hello message.
    pub fn hello(component_id: ComponentId, process_instance: ProcessInstanceId) -> Self {
        Self::Hello {
            component_id,
            process_instance,
            control_protocol_version: CONTROL_PROTOCOL_VERSION,
            snapshot_version: COMPOSER_SNAPSHOT_VERSION,
        }
    }

    /// Constructs a snapshot message.
    pub fn snapshot(snapshot: ComponentSnapshot) -> Self {
        Self::Snapshot { snapshot }
    }

    /// Constructs a current-version read-only inspection request.
    pub const fn inspect() -> Self {
        Self::Inspect {
            control_protocol_version: CONTROL_PROTOCOL_VERSION,
            snapshot_version: COMPOSER_SNAPSHOT_VERSION,
        }
    }

    /// Constructs an inspection request with explicit compatibility versions.
    pub const fn inspect_with_versions(
        control_protocol_version: u16,
        snapshot_version: u16,
    ) -> Self {
        Self::Inspect {
            control_protocol_version,
            snapshot_version,
        }
    }
}

/// A message sent by the composer to a component process.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ServerMessage {
    /// Requests activation with resolver-selected bindings.
    Activate {
        /// Composer revision that owns this command.
        revision: Revision,
        /// One resolved binding for every declared requirement.
        bindings: Vec<ResolvedBinding>,
    },
    /// Requests explicit shutdown of the current activation scope.
    Deactivate {
        /// Composer revision that owns this command.
        revision: Revision,
        /// Process identity expected to receive this command.
        expected_process_instance: ProcessInstanceId,
        /// Activation identity expected to receive this command.
        expected_activation: ActivationId,
        /// Human-readable reason for deactivation.
        reason: String,
    },
    /// Reports an ambiguous process identity conflict.
    ProcessConflict {
        /// Composer revision that owns this notification.
        revision: Revision,
        /// Stable identity for which two process lifetimes were observed.
        component_id: ComponentId,
        /// Process that was already connected.
        incumbent: ProcessInstanceId,
        /// Process that attempted to supersede the incumbent.
        contender: ProcessInstanceId,
    },
    /// Reports an incompatible control or snapshot protocol version.
    ///
    /// This diagnostic is deliberately sent before an incompatible peer is
    /// registered or any message from it is imported.
    ProtocolMismatch {
        /// Version required by the composer for control messages.
        expected_control_protocol_version: u16,
        /// Version required by the composer for component snapshots.
        expected_snapshot_version: u16,
        /// Version supplied by the peer, when it was present on the request.
        received_control_protocol_version: Option<u16>,
        /// Snapshot version supplied by the peer, when it was present on the request.
        received_snapshot_version: Option<u16>,
        /// Human-readable mismatch explanation.
        reason: String,
    },
    /// Returns a read-only diagnostic view of the composer.
    InspectResponse {
        /// Immutable point-in-time composer view.
        view: crate::composer::InspectionView,
    },
}

impl ServerMessage {
    /// Returns the revision carried by a lifecycle command/notification.
    ///
    /// Diagnostics are intentionally revisionless: consuming an inspection
    /// response must never perturb a component's command revision gate.
    pub const fn revision(&self) -> Option<Revision> {
        match self {
            Self::Activate { revision, .. }
            | Self::Deactivate { revision, .. }
            | Self::ProcessConflict { revision, .. } => Some(*revision),
            Self::ProtocolMismatch { .. } | Self::InspectResponse { .. } => None,
        }
    }
}

/// Convenience constructor used by tests and embedders.
pub const fn revision(epoch: ComposerEpoch, sequence: u64) -> Revision {
    Revision::new(epoch, sequence)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::{ActivationSnapshot, CapabilitySnapshot, RequirementSnapshot};
    use rbt_core::{
        CapabilityDescriptor, CapabilityInstanceId, ComponentState, ProvidedCapability,
        RequirementSpec, StreamDescriptor,
    };
    use rbt_types::{Frame, StreamProfile};

    fn sample_snapshot() -> ComponentSnapshot {
        let process_instance = ProcessInstanceId::from_uuid(uuid::Uuid::from_u128(4));
        let activation = ActivationId::new(2);
        let capability_instance = CapabilityInstanceId::from_uuid(uuid::Uuid::from_u128(5));
        let descriptor = CapabilityDescriptor::new(
            capability_instance,
            ComponentId::from("camera"),
            process_instance,
            activation,
            StreamDescriptor::for_fixed::<Frame>(
                capability_instance.stream_service_name(),
                StreamProfile::Sensor,
            ),
        );
        ComponentSnapshot::new(
            ComponentId::from("camera"),
            process_instance,
            ComponentState::Active,
            Some(ActivationSnapshot::new(activation, [])),
            [RequirementSnapshot::unbound(RequirementSpec::new(
                "clock", "clock.v1",
            ))],
            [CapabilitySnapshot::new(
                "Camera",
                capability_instance,
                descriptor,
            )],
        )
    }

    #[test]
    fn snapshot_protocol_round_trips_as_tagged_ndjson_objects() {
        let snapshot = sample_snapshot();
        let hello = ClientMessage::hello(snapshot.component_id.clone(), snapshot.process_instance);
        let encoded = serde_json::to_string(&hello).unwrap();
        assert_eq!(
            encoded,
            format!(
                r#"{{"type":"hello","component_id":"camera","process_instance":"00000000-0000-0000-0000-000000000004","control_protocol_version":{CONTROL_PROTOCOL_VERSION},"snapshot_version":{COMPOSER_SNAPSHOT_VERSION}}}"#
            )
        );
        assert_eq!(
            serde_json::from_str::<ClientMessage>(&encoded).unwrap(),
            hello
        );

        let snapshot_message = ClientMessage::snapshot(snapshot.clone());
        let line = format!("{}\n", serde_json::to_string(&snapshot_message).unwrap());
        assert_eq!(
            serde_json::from_str::<ClientMessage>(line.trim()).unwrap(),
            snapshot_message
        );

        let revision = Revision::new(ComposerEpoch::from_uuid(uuid::Uuid::from_u128(9)), 3);
        let activate = ServerMessage::Activate {
            revision,
            bindings: vec![],
        };
        let activate_json = serde_json::to_string(&activate).unwrap();
        assert_eq!(
            serde_json::from_str::<ServerMessage>(&activate_json).unwrap(),
            activate
        );

        let conflict = ServerMessage::ProcessConflict {
            revision,
            component_id: ComponentId::from("camera"),
            incumbent: snapshot.process_instance,
            contender: ProcessInstanceId::from_uuid(uuid::Uuid::from_u128(6)),
        };
        let conflict_json = serde_json::to_string(&conflict).unwrap();
        assert_eq!(
            serde_json::from_str::<ServerMessage>(&conflict_json).unwrap(),
            conflict
        );
    }

    #[allow(dead_code)]
    fn _provided_type_is_still_wire_compatible(capability: ProvidedCapability) {
        let _ = capability;
    }
}

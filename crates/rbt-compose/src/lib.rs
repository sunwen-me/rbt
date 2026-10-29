//! Composition primitives for the component runtime.
//!
//! It contains deterministic composition primitives plus the low-rate TCP
//! control-plane runner. Data-plane endpoints remain in `rbt-iox2`; this crate
//! only moves lifecycle metadata and capability endpoint descriptors.
//!
//! # Architecture invariants
//!
//! 1. Composer failure MUST NOT imply provider failure. Losing the control
//!    connection cannot tear down an otherwise-valid local activation.
//! 2. Capability existence MUST be backed by a live, valid activation.
//! 3. A critical task failure invalidates its activation, including every
//!    capability and data-plane endpoint owned by that activation.
//! 4. Composer restart MUST reconstruct reality, not recreate reality. A
//!    consistent recovered graph requires no lifecycle commands.

pub mod component;
pub mod composer;
pub mod effect_scope;
pub mod manifest;
pub mod protocol;
pub mod reconcile;
pub mod registry;

pub use component::{
    ActivationCtx, Component, ComponentError, ControlPlaneStatus, RequirementError,
    RpcEndpointError, RuntimeEvent, run_component, run_component_until,
};
pub use composer::{
    ActiveContext, CapabilityInspection, CapabilityTransportInspection, ComponentInspection,
    ComposerPhase, ControlConnectionStatus, ControlPlaneError, InspectionResponse, InspectionView,
    MAX_RPC_CLIENT_PRESSURE_OBSERVATIONS, MAX_SUBSCRIBER_PRESSURE_OBSERVATIONS,
    PRESSURE_FRESHNESS_WINDOW_MS, PendingInspection, PressureAssessment, PressureAssessmentReason,
    PressureFreshness, ProcessConflictInspection, RpcClientPressureObservation,
    SubscriberPressureObservation, assess_stream_pressure, queue_depth_for_profile,
    safe_overflow_for_profile, serve,
};
pub use effect_scope::{EffectScope, ScopedTaskHandle};
pub use manifest::{
    ActivationSnapshot, CapabilitySnapshot, ComponentManifest, ComponentSnapshot,
    RequirementSnapshot, SnapshotValidationError,
};
#[allow(deprecated)]
pub use protocol::{
    COMPOSER_SNAPSHOT_VERSION, CONTROL_PROTOCOL_VERSION, ClientMessage, PROTOCOL_VERSION,
    ServerMessage, TaskOutcome,
};
pub use reconcile::{ReconcilePlan, Reconciler, reconcile};
pub use registry::{CapabilityRegistry, RegisteredCapability, RegistryTopology};

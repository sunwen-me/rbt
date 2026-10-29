//! Runtime diagnostics for a running RBT composer.
//!
//! The CLI talks to the composer's read-only TCP inspection endpoint. It does
//! not inspect shared-memory directories or infer topology from local files.

use std::{
    collections::{BTreeMap, BTreeSet},
    fmt::Write as _,
    io,
    time::Duration,
};

use rbt_compose::{
    ActiveContext, CapabilityInspection, CapabilityTransportInspection, ClientMessage,
    ComponentInspection, InspectionView, PendingInspection, PressureAssessment, PressureFreshness,
    RpcClientPressureObservation, ServerMessage, SubscriberPressureObservation,
};
use rbt_core::{CapabilityTransportKind, ComponentState};
use serde::{Deserialize, Serialize};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader},
    net::TcpStream,
    time::timeout,
};
use uuid::Uuid;

mod recording;
pub use recording::{RecordOptions, RecordSummary, run_record};
mod playback;
pub use playback::{PlaySummary, run_play};
mod cli;
pub use cli::{Command, Invocation, command_help, parse_args, parse_invocation, usage};
pub mod doctor;
pub mod idl_command;
pub mod project;
pub mod runner;
pub mod tf_status;
pub mod time_status;
pub mod version;

/// Default composer control/inspection endpoint.
pub const DEFAULT_COMPOSER_ADDR: &str = "127.0.0.1:7447";

/// CLI errors.
#[derive(Debug, thiserror::Error)]
pub enum CliError {
    /// No command or an invalid command-line shape was supplied.
    #[error("{0}")]
    Usage(String),
    /// The composer could not be reached or queried.
    #[error("failed to inspect composer: {0}")]
    Inspect(#[source] std::io::Error),
    /// The composer returned a protocol diagnostic or unexpected response.
    #[error("composer inspection failed: {0}")]
    Protocol(String),
    /// A requested component or capability was not found.
    #[error("{0}")]
    NotFound(String),
    /// JSON encoding failed.
    #[error("failed to encode JSON: {0}")]
    Json(#[from] serde_json::Error),
    /// MCAP schema, codec, reader, or writer failure.
    #[error("MCAP operation failed: {0}")]
    Mcap(#[from] rbt_mcap::Error),
    /// Local shared-memory stream setup or operation failed.
    #[error("stream operation failed: {0}")]
    Stream(#[from] rbt_iox2::StreamError),
    /// The readiness WaitSet failed.
    #[error("recording wait failed: {0}")]
    Wait(#[from] rbt_iox2::WaitError),
    /// A bounded recording queue operation failed.
    #[error("recording queue failed: {0}")]
    Queue(#[from] rbt_mcap::QueueError),
    /// Recording stopped because completeness could not be preserved.
    #[error("recording failed: {0}")]
    Recording(String),
    /// A recording worker task panicked or was cancelled.
    #[error("recording worker failed: {0}")]
    Join(String),
    /// Native component runner/control-plane failure.
    #[error("runtime component failed: {0}")]
    Runtime(#[from] rbt_compose::ControlPlaneError),
    /// Playback scheduling, decode, or publication failed.
    #[error("playback failed: {0}")]
    Playback(String),
}

/// Fetch the immutable composer view over TCP+NDJSON.
pub async fn fetch_view(addr: impl Into<String>) -> Result<InspectionView, CliError> {
    fetch_view_with_timeout(addr.into(), Duration::from_secs(2)).await
}

#[derive(Debug)]
struct AddressedInspectError {
    address: String,
    source: io::Error,
}

impl std::fmt::Display for AddressedInspectError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            formatter,
            "Composer unavailable at {}: {}; start Composer or verify --composer/RBT_COMPOSER_ADDR/project config",
            self.address, self.source
        )
    }
}

impl std::error::Error for AddressedInspectError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        Some(&self.source)
    }
}

fn addressed_inspect_error(address: &str, source: io::Error) -> CliError {
    let kind = source.kind();
    CliError::Inspect(io::Error::new(
        kind,
        AddressedInspectError {
            address: address.to_owned(),
            source,
        },
    ))
}

/// Fetch an inspection view while bounding connect, request write, and response read.
///
/// The public inspection command uses a two-second limit. Keeping the duration
/// injectable makes the timeout behavior testable without slowing the test suite.
async fn fetch_view_with_timeout(
    addr: String,
    timeout_duration: Duration,
) -> Result<InspectionView, CliError> {
    match timeout(timeout_duration, fetch_view_once(&addr)).await {
        Ok(Ok(view)) => Ok(view),
        Ok(Err(CliError::Inspect(source))) => Err(addressed_inspect_error(&addr, source)),
        Ok(Err(error)) => Err(error),
        Err(_) => Err(addressed_inspect_error(
            &addr,
            io::Error::new(io::ErrorKind::TimedOut, "inspection request timed out"),
        )),
    }
}

async fn fetch_view_once(addr: &str) -> Result<InspectionView, CliError> {
    let stream = TcpStream::connect(addr).await.map_err(CliError::Inspect)?;
    let (read_half, mut write_half) = stream.into_split();
    let encoded = serde_json::to_vec(&ClientMessage::inspect())?;
    write_half
        .write_all(&encoded)
        .await
        .map_err(CliError::Inspect)?;
    write_half
        .write_all(b"\n")
        .await
        .map_err(CliError::Inspect)?;
    write_half.flush().await.map_err(CliError::Inspect)?;

    let mut reader = BufReader::new(read_half);
    let mut line = String::new();
    let read = reader
        .read_line(&mut line)
        .await
        .map_err(CliError::Inspect)?;
    if read == 0 {
        return Err(CliError::Protocol(
            "composer closed the inspection connection".to_owned(),
        ));
    }
    let response = serde_json::from_str::<ServerMessage>(line.trim_end())
        .map_err(|error| CliError::Protocol(format!("invalid inspection response: {error}")))?;
    match response {
        ServerMessage::InspectResponse { view } => Ok(view),
        ServerMessage::ProtocolMismatch { reason, .. } => Err(CliError::Protocol(reason)),
        other => Err(CliError::Protocol(format!(
            "unexpected response: {other:?}"
        ))),
    }
}

/// Render `rbt ps`.
pub fn format_ps(view: &InspectionView, full: bool) -> String {
    let mut output = String::new();
    let _ = writeln!(
        output,
        "COMPONENT\tSTATE\tPROCESS\tACTIVATION\tAGE\tCONTROL"
    );
    let _ = writeln!(
        output,
        "phase={} epoch={} revision={}",
        phase_name(view),
        display_uuid(view.epoch, full),
        view.revision.sequence
    );
    for component in sorted_components(view) {
        let snapshot = &component.snapshot;
        let activation = snapshot.activation.as_ref().map_or_else(
            || "-".to_owned(),
            |activation| activation.activation_id.to_string(),
        );
        let _ = writeln!(
            output,
            "{}\t{}\t{}\t{}\t{}\t{}",
            component.component_id,
            state_name(&snapshot.state),
            display_uuid(snapshot.process_instance, full),
            activation,
            lifecycle_age_label(component),
            connection_name(component)
        );
        if let Some(pending) = &component.pending {
            let _ = writeln!(
                output,
                "  pending={} ({})",
                pending.operation, pending.reason
            );
        }
        if full {
            let _ = writeln!(
                output,
                "  snapshot={}",
                if component.snapshot_received {
                    "received"
                } else {
                    "waiting"
                }
            );
            for requirement in &snapshot.requirements {
                let status = if requirement.binding.is_some() {
                    "satisfied"
                } else {
                    "missing"
                };
                let _ = writeln!(
                    output,
                    "  requirement {}: {} ({})",
                    requirement.requirement.requirement_name,
                    status,
                    requirement.requirement.capability_type
                );
            }
            if let ComponentState::Failed(reason) = &snapshot.state {
                let _ = writeln!(output, "  FAILED: {reason}");
            }
        }
    }
    output
}

/// Stable machine-readable `ps` projection. IDs are never shortened in JSON.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PsOutput {
    pub epoch: String,
    pub revision: u64,
    pub phase: String,
    pub components: Vec<PsComponentOutput>,
    pub process_conflicts: Vec<rbt_compose::ProcessConflictInspection>,
}

/// One component row in [`PsOutput`]. Lifecycle age remains absent because a
/// v3 snapshot does not carry transition timestamps.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PsComponentOutput {
    pub component_id: String,
    pub process_instance_id: String,
    pub activation_id: Option<u64>,
    pub state: String,
    pub block_reason: Option<String>,
    pub failure_reason: Option<String>,
    pub snapshot_received: bool,
    pub control_connection: String,
    pub lifecycle_age_ms: Option<u64>,
    pub lifecycle_freshness: String,
    pub pending: Option<PendingInspection>,
    pub stream_pressures: Vec<rbt_core::StreamPressure>,
    pub rpc_pressures: Vec<rbt_core::RpcPressure>,
}

fn ps_output(view: &InspectionView) -> PsOutput {
    PsOutput {
        epoch: view.epoch.to_string(),
        revision: view.revision.sequence,
        phase: phase_name(view).to_owned(),
        components: sorted_components(view)
            .into_iter()
            .map(|component| {
                let (block_reason, failure_reason) = match &component.snapshot.state {
                    ComponentState::Blocked(reason) => (Some(reason.to_string()), None),
                    ComponentState::Failed(reason) => (None, Some(reason.clone())),
                    _ => (None, None),
                };
                PsComponentOutput {
                    component_id: component.component_id.to_string(),
                    process_instance_id: component.snapshot.process_instance.to_string(),
                    activation_id: component
                        .snapshot
                        .activation
                        .as_ref()
                        .map(|activation| activation.activation_id.value()),
                    state: state_name(&component.snapshot.state).to_owned(),
                    block_reason,
                    failure_reason,
                    snapshot_received: component.snapshot_received,
                    control_connection: connection_name(component).to_owned(),
                    lifecycle_age_ms: None,
                    lifecycle_freshness: lifecycle_freshness(component).to_owned(),
                    pending: component.pending.clone(),
                    stream_pressures: component.snapshot.stream_pressures.clone(),
                    rpc_pressures: component.snapshot.rpc_pressures.clone(),
                }
            })
            .collect(),
        process_conflicts: view.process_conflicts.clone(),
    }
}

/// Render a machine-readable `rbt ps --json` response.
pub fn format_ps_json(view: &InspectionView) -> Result<String, CliError> {
    format_json(&ps_output(view))
}

/// Stable machine-readable diagnostic reason codes.
///
/// These codes intentionally describe only facts available in the v3
/// inspection snapshot.  In particular, a runtime-reported failure is kept
/// generic when the snapshot does not contain enough evidence to distinguish
/// an activation error from another runtime failure.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum DiagnosticReasonCode {
    RequirementUnresolved,
    ProviderFailed,
    ProviderDisappeared,
    ActivationFailed,
    CriticalTaskFailed,
    PressureLoanCapacity,
    PressureBorrowCapacity,
    PressureControlDelivery,
    MetricsStale,
    RuntimeReported,
    RpcServerUnavailable,
    RpcDeadlineExceeded,
    RpcClientCapacity,
    RpcServerCapacity,
    RpcCancelled,
    RpcTransportFailure,
}

/// Short alias retained for embedders that refer to these as reason codes.
pub type ReasonCode = DiagnosticReasonCode;

/// The kind of node in the structured capability graph.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum GraphNodeKind {
    Component,
    Requirement,
    Capability,
}

/// The kind of relationship in the structured capability graph.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum GraphEdgeKind {
    Requires,
    Binds,
    Provides,
}

/// A truthful freshness summary for a projection.
///
/// The Composer inspection response is a live response, but v3 does not carry
/// lifecycle transition timestamps.  Therefore `lifecycle_age_ms` is always
/// `None`; it must not be estimated from the inspection revision or process
/// identity.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ProjectionFreshness {
    pub source: String,
    pub live_composer_response: bool,
    pub composer_phase: String,
    pub composer_epoch: String,
    pub revision: u64,
    pub snapshot_received: Option<bool>,
    pub control_connection: Option<String>,
    pub lifecycle_age_ms: Option<u64>,
    pub lifecycle_freshness: String,
    pub components: Vec<ComponentFreshnessOutput>,
}

/// Per-component freshness facts included in graph and why projections.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ComponentFreshnessOutput {
    pub component_id: String,
    pub snapshot_received: bool,
    pub control_connection: String,
    pub lifecycle_age_ms: Option<u64>,
    pub lifecycle_freshness: String,
}

/// One typed graph node.  Identity fields are intentionally full-width even
/// when the human formatter shortens UUIDs.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct GraphNodeOutput {
    pub id: String,
    pub kind: GraphNodeKind,
    pub label: String,
    pub status: String,
    pub resolved: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub component_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub state: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub requirement_name: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub capability_type: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub capability_instance: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub provider_component: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub provider_process: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub activation: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub physical_endpoint: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub transport_kind: Option<CapabilityTransportKind>,
}

/// One typed graph edge, including whether the relation is currently
/// resolved against the active Composer topology.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct GraphEdgeOutput {
    pub source: String,
    pub target: String,
    pub kind: GraphEdgeKind,
    pub resolved: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub requirement_name: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub capability_type: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub transport_kind: Option<CapabilityTransportKind>,
}

/// Structured capability graph projection.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct GraphOutput {
    pub epoch: String,
    pub revision: u64,
    pub phase: String,
    pub nodes: Vec<GraphNodeOutput>,
    pub edges: Vec<GraphEdgeOutput>,
    pub freshness: ProjectionFreshness,
}

#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
struct GraphCapabilityRecord {
    capability_type: String,
    capability_instance: String,
    provider_component: String,
    provider_process: String,
    activation: String,
    physical_endpoint: String,
    active: bool,
    transport_kind: CapabilityTransportKind,
}

fn component_freshness(component: &ComponentInspection) -> ComponentFreshnessOutput {
    ComponentFreshnessOutput {
        component_id: component.component_id.to_string(),
        snapshot_received: component.snapshot_received,
        control_connection: connection_name(component).to_owned(),
        lifecycle_age_ms: None,
        lifecycle_freshness: lifecycle_freshness(component).to_owned(),
    }
}

fn projection_freshness(
    view: &InspectionView,
    component: Option<&ComponentInspection>,
) -> ProjectionFreshness {
    let freshness_status = component.map_or_else(
        || {
            if sorted_components(view)
                .iter()
                .any(|component| lifecycle_freshness(component) == "stale_age_unavailable")
            {
                "stale_age_unavailable"
            } else {
                "age_unavailable"
            }
        },
        lifecycle_freshness,
    );
    ProjectionFreshness {
        source: "live_composer_response".to_owned(),
        live_composer_response: true,
        composer_phase: phase_name(view).to_owned(),
        composer_epoch: view.epoch.to_string(),
        revision: view.revision.sequence,
        snapshot_received: component.map(|component| component.snapshot_received),
        control_connection: component.map(|component| connection_name(component).to_owned()),
        lifecycle_age_ms: None,
        lifecycle_freshness: freshness_status.to_owned(),
        components: sorted_components(view)
            .iter()
            .map(|component| component_freshness(component))
            .collect(),
    }
}

fn sorted_components(view: &InspectionView) -> Vec<&ComponentInspection> {
    let mut components = view.components.iter().collect::<Vec<_>>();
    components.sort_by(|left, right| left.component_id.cmp(&right.component_id));
    components
}

fn topology_capability_matches(
    capability: &CapabilityInspection,
    capability_type: &str,
    provider: &str,
    capability_instance: &rbt_core::CapabilityInstanceId,
) -> bool {
    capability.capability_type == capability_type
        && capability.provider_component.as_ref() == provider
        && capability.capability_instance == *capability_instance
}

fn binding_is_active(view: &InspectionView, binding: &rbt_core::ResolvedBinding) -> bool {
    view.capability_topology.iter().any(|capability| {
        topology_capability_matches(
            capability,
            &binding.capability_type,
            binding.provider.as_ref(),
            &binding.descriptor.capability_instance,
        )
    })
}

fn graph_capability_record_from_inspection(
    capability: &CapabilityInspection,
) -> GraphCapabilityRecord {
    GraphCapabilityRecord {
        capability_type: capability.capability_type.clone(),
        capability_instance: capability.capability_instance.to_string(),
        provider_component: capability.provider_component.to_string(),
        provider_process: capability.provider_process.to_string(),
        activation: capability.activation.to_string(),
        physical_endpoint: capability.physical_endpoint.clone(),
        active: true,
        transport_kind: capability.transport_kind(),
    }
}

fn graph_capability_record_from_snapshot(
    capability_type: &str,
    descriptor: &rbt_core::CapabilityDescriptor,
) -> GraphCapabilityRecord {
    GraphCapabilityRecord {
        capability_type: capability_type.to_owned(),
        capability_instance: descriptor.capability_instance.to_string(),
        provider_component: descriptor.provider_component.to_string(),
        provider_process: descriptor.provider_process.to_string(),
        activation: descriptor.activation.to_string(),
        physical_endpoint: descriptor.service_name().to_owned(),
        active: false,
        transport_kind: descriptor.transport_kind(),
    }
}

fn graph_projection(view: &InspectionView) -> GraphOutput {
    let mut nodes = Vec::new();
    let mut edges = Vec::new();
    let mut capabilities = BTreeMap::<String, GraphCapabilityRecord>::new();

    for capability in &view.capability_topology {
        let record = graph_capability_record_from_inspection(capability);
        capabilities.insert(record.capability_instance.clone(), record);
    }
    for component in &view.components {
        for capability in &component.snapshot.provided_capabilities {
            let record = graph_capability_record_from_snapshot(
                &capability.capability_type,
                &capability.descriptor,
            );
            capabilities
                .entry(record.capability_instance.clone())
                .and_modify(|existing| {
                    if !existing.active {
                        *existing = record.clone();
                    }
                })
                .or_insert(record);
        }
        for requirement in &component.snapshot.requirements {
            if let Some(binding) = &requirement.binding {
                let record = graph_capability_record_from_snapshot(
                    &binding.capability_type,
                    &binding.descriptor,
                );
                capabilities
                    .entry(record.capability_instance.clone())
                    .and_modify(|existing| {
                        if !existing.active {
                            *existing = record.clone();
                        }
                    })
                    .or_insert(record);
            }
        }
    }

    for component in sorted_components(view) {
        let component_id = component.component_id.to_string();
        let component_node_id = graph_component_node_id(&component_id);
        nodes.push(GraphNodeOutput {
            id: component_node_id.clone(),
            kind: GraphNodeKind::Component,
            label: component_id.clone(),
            status: "observed".to_owned(),
            resolved: true,
            component_id: Some(component_id.clone()),
            state: Some(state_name(&component.snapshot.state).to_ascii_lowercase()),
            requirement_name: None,
            capability_type: None,
            capability_instance: None,
            provider_component: None,
            provider_process: Some(component.snapshot.process_instance.to_string()),
            activation: component
                .snapshot
                .activation
                .as_ref()
                .map(|activation| activation.activation_id.to_string()),
            physical_endpoint: None,
            transport_kind: None,
        });

        let mut requirements = component.snapshot.requirements.iter().collect::<Vec<_>>();
        requirements.sort_by(|left, right| {
            left.requirement
                .requirement_name
                .cmp(&right.requirement.requirement_name)
                .then_with(|| {
                    left.requirement
                        .capability_type
                        .cmp(&right.requirement.capability_type)
                })
        });
        for requirement in requirements {
            let requirement_id =
                graph_requirement_node_id(&component_id, &requirement.requirement.requirement_name);
            let resolved = requirement
                .binding
                .as_ref()
                .is_some_and(|binding| binding_is_active(view, binding));
            nodes.push(GraphNodeOutput {
                id: requirement_id.clone(),
                kind: GraphNodeKind::Requirement,
                label: format!(
                    "{}:{}",
                    requirement.requirement.requirement_name,
                    requirement.requirement.capability_type
                ),
                status: if resolved {
                    "resolved".to_owned()
                } else {
                    "unresolved".to_owned()
                },
                resolved,
                component_id: Some(component_id.clone()),
                state: None,
                requirement_name: Some(requirement.requirement.requirement_name.clone()),
                capability_type: Some(requirement.requirement.capability_type.clone()),
                capability_instance: requirement
                    .binding
                    .as_ref()
                    .map(|binding| binding.descriptor.capability_instance.to_string()),
                provider_component: requirement
                    .binding
                    .as_ref()
                    .map(|binding| binding.provider.to_string()),
                provider_process: None,
                activation: None,
                physical_endpoint: requirement
                    .binding
                    .as_ref()
                    .map(|binding| binding.descriptor.service_name().to_owned()),
                transport_kind: requirement
                    .binding
                    .as_ref()
                    .map(|binding| binding.descriptor.transport_kind()),
            });
            edges.push(GraphEdgeOutput {
                source: component_node_id.clone(),
                target: requirement_id.clone(),
                kind: GraphEdgeKind::Requires,
                resolved,
                requirement_name: Some(requirement.requirement.requirement_name.clone()),
                capability_type: Some(requirement.requirement.capability_type.clone()),
                transport_kind: requirement
                    .binding
                    .as_ref()
                    .map(|binding| binding.descriptor.transport_kind()),
            });
            if let Some(binding) = &requirement.binding {
                let capability_id =
                    graph_capability_node_id(&binding.descriptor.capability_instance);
                edges.push(GraphEdgeOutput {
                    source: requirement_id,
                    target: capability_id,
                    kind: GraphEdgeKind::Binds,
                    resolved,
                    requirement_name: Some(requirement.requirement.requirement_name.clone()),
                    capability_type: Some(binding.capability_type.clone()),
                    transport_kind: Some(binding.descriptor.transport_kind()),
                });
            }
        }
    }

    let mut observed_components = nodes
        .iter()
        .filter_map(|node| {
            (node.kind == GraphNodeKind::Component)
                .then_some(node.component_id.clone())
                .flatten()
        })
        .collect::<BTreeSet<_>>();
    for record in capabilities.values() {
        if !observed_components.contains(&record.provider_component) {
            nodes.push(GraphNodeOutput {
                id: graph_component_node_id(&record.provider_component),
                kind: GraphNodeKind::Component,
                label: record.provider_component.clone(),
                status: "topology".to_owned(),
                resolved: record.active,
                component_id: Some(record.provider_component.clone()),
                state: record.active.then(|| "active".to_owned()),
                requirement_name: None,
                capability_type: None,
                capability_instance: None,
                provider_component: None,
                provider_process: Some(record.provider_process.clone()),
                activation: Some(record.activation.clone()),
                physical_endpoint: None,
                transport_kind: None,
            });
            observed_components.insert(record.provider_component.clone());
        }
        let capability_id = graph_capability_node_id_from_string(&record.capability_instance);
        nodes.push(GraphNodeOutput {
            id: capability_id.clone(),
            kind: GraphNodeKind::Capability,
            label: format!("{}:{}", record.capability_type, record.capability_instance),
            status: if record.active {
                "active".to_owned()
            } else {
                "retained".to_owned()
            },
            resolved: record.active,
            component_id: None,
            state: None,
            requirement_name: None,
            capability_type: Some(record.capability_type.clone()),
            capability_instance: Some(record.capability_instance.clone()),
            provider_component: Some(record.provider_component.clone()),
            provider_process: Some(record.provider_process.clone()),
            activation: Some(record.activation.clone()),
            physical_endpoint: Some(record.physical_endpoint.clone()),
            transport_kind: Some(record.transport_kind),
        });
        let provider_id = graph_component_node_id(&record.provider_component);
        edges.push(GraphEdgeOutput {
            source: provider_id,
            target: capability_id,
            kind: GraphEdgeKind::Provides,
            resolved: record.active,
            requirement_name: None,
            capability_type: Some(record.capability_type.clone()),
            transport_kind: Some(record.transport_kind),
        });
    }

    nodes.sort_by(|left, right| {
        left.id
            .cmp(&right.id)
            .then_with(|| left.kind.cmp(&right.kind))
    });
    edges.sort_by(|left, right| {
        left.source
            .cmp(&right.source)
            .then_with(|| left.target.cmp(&right.target))
            .then_with(|| left.kind.cmp(&right.kind))
            .then_with(|| left.resolved.cmp(&right.resolved))
    });

    GraphOutput {
        epoch: view.epoch.to_string(),
        revision: view.revision.sequence,
        phase: phase_name(view).to_owned(),
        nodes,
        edges,
        freshness: projection_freshness(view, None),
    }
}

/// Render the structured capability graph as JSON.
pub fn format_graph_json(view: &InspectionView) -> Result<String, CliError> {
    format_json(&graph_projection(view))
}

/// Render the capability graph as text or DOT.
pub fn format_graph(view: &InspectionView, dot: bool) -> String {
    if dot {
        return format_graph_dot(view);
    }
    let graph = graph_projection(view);
    let mut output = String::new();
    for node in graph
        .nodes
        .iter()
        .filter(|node| node.kind == GraphNodeKind::Component)
    {
        let _ = writeln!(
            output,
            "component {} [{}]",
            node.component_id.as_deref().unwrap_or(&node.label),
            node.state
                .as_deref()
                .unwrap_or("unknown")
                .to_ascii_uppercase()
        );
        let mut requirements = graph
            .nodes
            .iter()
            .filter(|requirement| {
                requirement.kind == GraphNodeKind::Requirement
                    && requirement.component_id.as_deref() == node.component_id.as_deref()
            })
            .collect::<Vec<_>>();
        requirements.sort_by(|left, right| left.id.cmp(&right.id));
        for requirement in requirements {
            let _ = writeln!(
                output,
                "  requirement {} ({} [{}]): {}",
                requirement.requirement_name.as_deref().unwrap_or("-"),
                requirement.capability_type.as_deref().unwrap_or("-"),
                transport_label(requirement.transport_kind),
                if requirement.resolved {
                    "resolved"
                } else {
                    "UNRESOLVED"
                }
            );
        }
    }

    let mut rendered_edges = graph
        .edges
        .iter()
        .filter(|edge| edge.kind == GraphEdgeKind::Binds && edge.resolved)
        .collect::<Vec<_>>();
    rendered_edges.sort_by(|left, right| {
        left.source
            .cmp(&right.source)
            .then_with(|| left.target.cmp(&right.target))
    });
    for edge in &rendered_edges {
        let Some(requirement) = graph.nodes.iter().find(|node| node.id == edge.source) else {
            continue;
        };
        let Some(component) = requirement.component_id.as_deref() else {
            continue;
        };
        let Some(capability) = graph.nodes.iter().find(|node| node.id == edge.target) else {
            continue;
        };
        let _ = writeln!(
            output,
            "{} -[{}:{}:{}]-> {}",
            capability.provider_component.as_deref().unwrap_or("-"),
            requirement.requirement_name.as_deref().unwrap_or("-"),
            requirement.capability_type.as_deref().unwrap_or("-"),
            transport_label(edge.transport_kind),
            component
        );
    }
    if rendered_edges.is_empty() {
        output.push_str("(no satisfied requirement edges)\n");
    }
    if output.is_empty() {
        output.push_str("(no components)\n");
    }
    output
}

fn graph_component_node_id(component: &str) -> String {
    format!("component:{component}")
}

fn graph_requirement_node_id(component: &str, requirement: &str) -> String {
    format!("requirement:{component}:{requirement}")
}

fn graph_capability_node_id(capability: &rbt_core::CapabilityInstanceId) -> String {
    graph_capability_node_id_from_string(&capability.to_string())
}

fn graph_capability_node_id_from_string(capability: &str) -> String {
    format!("capability:{capability}")
}

/// Render the capability graph in deterministic Graphviz DOT form.
pub fn format_graph_dot(view: &InspectionView) -> String {
    let graph = graph_projection(view);
    let mut output = String::from("digraph rbt {\n");
    for node in &graph.nodes {
        let shape = match node.kind {
            GraphNodeKind::Component => "box",
            GraphNodeKind::Requirement => "ellipse",
            GraphNodeKind::Capability => "diamond",
        };
        let status = if node.resolved {
            node.status.as_str()
        } else {
            "unresolved"
        };
        let _ = writeln!(
            output,
            "  \"{}\" [label=\"{} [{}]\", shape={}];",
            dot_escape(&node.id),
            dot_escape(&node.label),
            dot_escape(status),
            shape
        );
    }
    for edge in &graph.edges {
        let label = match edge.kind {
            GraphEdgeKind::Requires => format!(
                "requires:{}",
                edge.capability_type.as_deref().unwrap_or("-")
            ),
            GraphEdgeKind::Binds => format!(
                "binds:{}:{}",
                edge.requirement_name.as_deref().unwrap_or("-"),
                edge.capability_type.as_deref().unwrap_or("-")
            ),
            GraphEdgeKind::Provides => format!(
                "provides:{}",
                edge.capability_type.as_deref().unwrap_or("-")
            ),
        };
        let _ = writeln!(
            output,
            "  \"{}\" -> \"{}\" [label=\"{}\"{}];",
            dot_escape(&edge.source),
            dot_escape(&edge.target),
            dot_escape(&label),
            if edge.resolved {
                ""
            } else {
                ", style=dashed, color=red"
            }
        );
    }
    output.push_str("}\n");
    output
}

/// One stable reason with optional evidence copied from the inspection
/// snapshot.  `code` is the field consumers should key on; `detail` is for
/// operator display and is never used to invent a cause.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct WhyReasonOutput {
    pub code: DiagnosticReasonCode,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub detail: Option<String>,
}

/// A current active provider candidate for one requirement.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct WhyProviderCandidateOutput {
    pub provider: String,
    pub capability_type: String,
    pub capability_instance: String,
    pub provider_process: String,
    pub activation: String,
    pub physical_endpoint: String,
    pub state: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub failure_reason: Option<String>,
    pub active: bool,
    pub transport_kind: CapabilityTransportKind,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub request_type: Option<rbt_core::MessageTypeDescriptor>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub response_type: Option<rbt_core::MessageTypeDescriptor>,
}

/// A retained or currently installed requirement binding.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct WhyBindingOutput {
    pub provider: String,
    pub capability_type: String,
    pub capability_instance: String,
    pub provider_process: String,
    pub activation: String,
    pub physical_endpoint: String,
    pub active: bool,
    pub transport_kind: CapabilityTransportKind,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub request_type: Option<rbt_core::MessageTypeDescriptor>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub response_type: Option<rbt_core::MessageTypeDescriptor>,
}

/// Structured requirement evidence in a why projection.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct WhyRequirementOutput {
    pub requirement_name: String,
    pub capability_type: String,
    pub resolved: bool,
    pub binding: Option<WhyBindingOutput>,
    pub candidates: Vec<WhyProviderCandidateOutput>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub transport_kind: Option<CapabilityTransportKind>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub request_type: Option<rbt_core::MessageTypeDescriptor>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub response_type: Option<rbt_core::MessageTypeDescriptor>,
}

/// Pressure evidence relevant to the selected component.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct WhyPressureOutput {
    pub capability_type: String,
    pub capability_instance: String,
    pub provider: String,
    pub provider_process: String,
    pub activation: String,
    pub physical_endpoint: String,
    pub pressure_available: bool,
    pub pressure_freshness: Option<PressureFreshness>,
    pub assessments: Vec<PressureAssessment>,
    pub subscriber_observations: Vec<SubscriberPressureObservation>,
}

/// Structured explanation tree for one component.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct WhyOutput {
    pub component: String,
    pub component_state: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub state_detail: Option<String>,
    pub control_connection: String,
    pub snapshot_received: bool,
    pub pending: Option<PendingInspection>,
    pub reasons: Vec<WhyReasonOutput>,
    pub reason_codes: Vec<DiagnosticReasonCode>,
    pub requirements: Vec<WhyRequirementOutput>,
    pub pressure_assessments: Vec<WhyPressureOutput>,
    pub suggestions: Vec<String>,
    pub freshness: ProjectionFreshness,
    pub last_active: Option<ActiveContext>,
    pub process_conflict: Option<rbt_compose::ProcessConflictInspection>,
}

fn state_detail(state: &ComponentState) -> Option<String> {
    match state {
        ComponentState::Blocked(reason) => Some(reason.to_string()),
        ComponentState::Failed(reason) => Some(reason.clone()),
        _ => None,
    }
}

fn candidate_state(
    view: &InspectionView,
    provider: &rbt_core::ComponentId,
) -> (String, Option<String>) {
    view.components
        .iter()
        .find(|component| component.component_id == *provider)
        .map_or_else(
            || ("ACTIVE".to_owned(), None),
            |component| {
                (
                    state_name(&component.snapshot.state).to_owned(),
                    match &component.snapshot.state {
                        ComponentState::Failed(reason) => Some(reason.clone()),
                        _ => None,
                    },
                )
            },
        )
}

fn why_candidate(
    view: &InspectionView,
    capability: &CapabilityInspection,
) -> WhyProviderCandidateOutput {
    let (state, failure_reason) = candidate_state(view, &capability.provider_component);
    let (transport_kind, request_type, response_type) = match &capability.transport {
        CapabilityTransportInspection::Stream { .. } => {
            (CapabilityTransportKind::Stream, None, None)
        }
        CapabilityTransportInspection::Rpc {
            request_type,
            response_type,
            ..
        } => (
            CapabilityTransportKind::Rpc,
            Some(request_type.clone()),
            Some(response_type.clone()),
        ),
    };
    WhyProviderCandidateOutput {
        provider: capability.provider_component.to_string(),
        capability_type: capability.capability_type.clone(),
        capability_instance: capability.capability_instance.to_string(),
        provider_process: capability.provider_process.to_string(),
        activation: capability.activation.to_string(),
        physical_endpoint: capability.physical_endpoint.clone(),
        state,
        failure_reason,
        active: true,
        transport_kind,
        request_type,
        response_type,
    }
}

fn provider_candidates(
    view: &InspectionView,
    capability_type: &str,
) -> Vec<WhyProviderCandidateOutput> {
    let mut candidates = BTreeMap::<String, WhyProviderCandidateOutput>::new();
    for capability in view
        .capability_topology
        .iter()
        .filter(|capability| capability.capability_type == capability_type)
    {
        let candidate = why_candidate(view, capability);
        candidates.insert(candidate.capability_instance.clone(), candidate);
    }
    for component in &view.components {
        if !matches!(component.snapshot.state, ComponentState::Failed(_)) {
            continue;
        }
        let retained = component
            .snapshot
            .provided_capabilities
            .iter()
            .filter(|provided| provided.capability_type == capability_type)
            .map(|provided| (&provided.capability_type, &provided.descriptor));
        for (candidate_type, descriptor) in retained {
            let instance = descriptor.capability_instance.to_string();
            let (state, failure_reason) = candidate_state(view, &descriptor.provider_component);
            candidates
                .entry(instance.clone())
                .or_insert_with(|| WhyProviderCandidateOutput {
                    provider: descriptor.provider_component.to_string(),
                    capability_type: candidate_type.clone(),
                    capability_instance: instance,
                    provider_process: descriptor.provider_process.to_string(),
                    activation: descriptor.activation.to_string(),
                    physical_endpoint: descriptor.service_name().to_owned(),
                    state,
                    failure_reason,
                    active: false,
                    transport_kind: descriptor.transport_kind(),
                    request_type: descriptor.try_rpc().map(|rpc| rpc.request_type().clone()),
                    response_type: descriptor.try_rpc().map(|rpc| rpc.response_type().clone()),
                });
        }
        if let Some(active) = &component.last_active {
            for capability in active
                .capabilities
                .iter()
                .filter(|capability| capability.capability_type == capability_type)
            {
                let instance = capability.capability_instance.to_string();
                let (state, failure_reason) = candidate_state(view, &capability.provider_component);
                candidates
                    .entry(instance.clone())
                    .or_insert_with(|| WhyProviderCandidateOutput {
                        provider: capability.provider_component.to_string(),
                        capability_type: capability.capability_type.clone(),
                        capability_instance: instance,
                        provider_process: capability.provider_process.to_string(),
                        activation: capability.activation.to_string(),
                        physical_endpoint: capability.physical_endpoint.clone(),
                        state,
                        failure_reason,
                        active: false,
                        transport_kind: capability.transport_kind(),
                        request_type: match &capability.transport {
                            CapabilityTransportInspection::Rpc { request_type, .. } => {
                                Some(request_type.clone())
                            }
                            CapabilityTransportInspection::Stream { .. } => None,
                        },
                        response_type: match &capability.transport {
                            CapabilityTransportInspection::Rpc { response_type, .. } => {
                                Some(response_type.clone())
                            }
                            CapabilityTransportInspection::Stream { .. } => None,
                        },
                    });
            }
        }
    }
    candidates.into_values().collect()
}

fn why_binding(binding: &rbt_core::ResolvedBinding, active: bool) -> WhyBindingOutput {
    let transport_kind = binding.descriptor.transport_kind();
    let request_type = binding
        .descriptor
        .try_rpc()
        .map(|rpc| rpc.request_type().clone());
    let response_type = binding
        .descriptor
        .try_rpc()
        .map(|rpc| rpc.response_type().clone());
    WhyBindingOutput {
        provider: binding.provider.to_string(),
        capability_type: binding.capability_type.clone(),
        capability_instance: binding.descriptor.capability_instance.to_string(),
        provider_process: binding.descriptor.provider_process.to_string(),
        activation: binding.descriptor.activation.to_string(),
        physical_endpoint: binding.descriptor.service_name().to_owned(),
        active,
        transport_kind,
        request_type,
        response_type,
    }
}

fn requirement_output(
    view: &InspectionView,
    component: &ComponentInspection,
    requirement: &rbt_compose::RequirementSnapshot,
) -> WhyRequirementOutput {
    let current_binding = requirement.binding.as_ref();
    let retained_binding = component.last_active.as_ref().and_then(|active| {
        active.bindings.iter().find(|binding| {
            binding.requirement_name == requirement.requirement.requirement_name
                && binding.capability_type == requirement.requirement.capability_type
        })
    });
    let resolved = current_binding.is_some_and(|binding| binding_is_active(view, binding));
    let displayed_binding = current_binding.or(retained_binding);
    let candidates = provider_candidates(view, &requirement.requirement.capability_type);
    let context = displayed_binding
        .map(|binding| {
            (
                Some(binding.descriptor.transport_kind()),
                binding
                    .descriptor
                    .try_rpc()
                    .map(|rpc| rpc.request_type().clone()),
                binding
                    .descriptor
                    .try_rpc()
                    .map(|rpc| rpc.response_type().clone()),
            )
        })
        .or_else(|| {
            let first = candidates.first()?;
            Some((
                Some(first.transport_kind),
                first.request_type.clone(),
                first.response_type.clone(),
            ))
        })
        .unwrap_or((None, None, None));
    WhyRequirementOutput {
        requirement_name: requirement.requirement.requirement_name.clone(),
        capability_type: requirement.requirement.capability_type.clone(),
        resolved,
        binding: displayed_binding.map(|binding| why_binding(binding, resolved)),
        candidates,
        transport_kind: context.0,
        request_type: context.1,
        response_type: context.2,
    }
}

fn relevant_pressure<'a>(
    view: &'a InspectionView,
    component: &ComponentInspection,
) -> Vec<&'a CapabilityInspection> {
    let component_id = component.component_id.as_ref();
    let mut capabilities = view
        .capability_topology
        .iter()
        .filter(|capability| {
            capability.provider_component.as_ref() == component_id
                || component.snapshot.requirements.iter().any(|requirement| {
                    requirement.binding.as_ref().is_some_and(|binding| {
                        binding.descriptor.capability_instance == capability.capability_instance
                    })
                })
        })
        .filter(|capability| match &capability.transport {
            CapabilityTransportInspection::Stream {
                assessments,
                pressure,
                pressure_freshness,
                subscriber_observations,
                ..
            } => {
                !assessments.is_empty()
                    || pressure.is_some()
                    || pressure_freshness.is_some()
                    || subscriber_observations.iter().any(|observation| {
                        !observation.fresh || !observation.assessments.is_empty()
                    })
            }
            CapabilityTransportInspection::Rpc { .. } => false,
        })
        .collect::<Vec<_>>();
    capabilities.sort_by(|left, right| {
        left.capability_type
            .cmp(&right.capability_type)
            .then_with(|| left.capability_instance.cmp(&right.capability_instance))
    });
    capabilities
}

fn why_pressure(capability: &CapabilityInspection) -> Option<WhyPressureOutput> {
    let CapabilityTransportInspection::Stream {
        pressure,
        pressure_freshness,
        assessments,
        subscriber_observations,
        ..
    } = &capability.transport
    else {
        return None;
    };
    Some(WhyPressureOutput {
        capability_type: capability.capability_type.clone(),
        capability_instance: capability.capability_instance.to_string(),
        provider: capability.provider_component.to_string(),
        provider_process: capability.provider_process.to_string(),
        activation: capability.activation.to_string(),
        physical_endpoint: capability.physical_endpoint.clone(),
        pressure_available: pressure.is_some(),
        pressure_freshness: *pressure_freshness,
        assessments: assessments.clone(),
        subscriber_observations: subscriber_observations.clone(),
    })
}

fn pressure_reason_code(assessment: &PressureAssessment) -> DiagnosticReasonCode {
    match assessment.reason {
        rbt_compose::PressureAssessmentReason::LoanCapacityReached => {
            DiagnosticReasonCode::PressureLoanCapacity
        }
        rbt_compose::PressureAssessmentReason::BorrowCapacityReached
        | rbt_compose::PressureAssessmentReason::OutstandingBorrow => {
            DiagnosticReasonCode::PressureBorrowCapacity
        }
        rbt_compose::PressureAssessmentReason::ControlDeliveryPressure
        | rbt_compose::PressureAssessmentReason::ReadinessFailuresObserved => {
            DiagnosticReasonCode::PressureControlDelivery
        }
    }
}

fn state_reason_code(state: &ComponentState) -> Option<DiagnosticReasonCode> {
    let detail = state_detail(state)?;
    let lower = detail.to_ascii_lowercase();
    match state {
        ComponentState::Failed(_)
            if (lower.contains("critical task") || lower.contains("critical_task"))
                && (lower.contains("fail")
                    || lower.contains("panic")
                    || lower.contains("exit")) =>
        {
            Some(DiagnosticReasonCode::CriticalTaskFailed)
        }
        ComponentState::Failed(_)
            if lower.contains("activation")
                && (lower.contains("fail") || lower.contains("error")) =>
        {
            Some(DiagnosticReasonCode::ActivationFailed)
        }
        ComponentState::Failed(_) | ComponentState::Blocked(_) => {
            Some(DiagnosticReasonCode::RuntimeReported)
        }
        _ => None,
    }
}

fn why_reasons(
    view: &InspectionView,
    component: &ComponentInspection,
    requirements: &[WhyRequirementOutput],
    pressure: &[WhyPressureOutput],
) -> Vec<WhyReasonOutput> {
    let mut reasons = BTreeMap::<DiagnosticReasonCode, Option<String>>::new();
    if requirements.iter().any(|requirement| !requirement.resolved) {
        reasons.insert(
            DiagnosticReasonCode::RequirementUnresolved,
            Some("one or more declared requirements have no current installed binding".to_owned()),
        );
    }
    let failed_providers = requirements
        .iter()
        .filter(|requirement| !requirement.resolved)
        .flat_map(|requirement| requirement.candidates.iter())
        .filter(|candidate| candidate.state == "FAILED")
        .map(|candidate| candidate.provider.clone())
        .collect::<BTreeSet<_>>();
    if !failed_providers.is_empty() {
        reasons.insert(
            DiagnosticReasonCode::ProviderFailed,
            Some(format!(
                "known provider candidate(s) are FAILED: {}",
                failed_providers.into_iter().collect::<Vec<_>>().join(", ")
            )),
        );
    }
    if matches!(component.snapshot.state, ComponentState::Blocked(_))
        && component.last_active.as_ref().is_some_and(|active| {
            active
                .bindings
                .iter()
                .any(|binding| !binding_is_active(view, binding))
        })
    {
        reasons.insert(
            DiagnosticReasonCode::ProviderDisappeared,
            Some("a retained binding is not present in the active topology".to_owned()),
        );
    }
    if let Some(code) = state_reason_code(&component.snapshot.state) {
        let detail = state_detail(&component.snapshot.state);
        reasons.insert(code, detail);
    }
    for assessment in pressure.iter().flat_map(|pressure| {
        pressure.assessments.iter().chain(
            pressure
                .subscriber_observations
                .iter()
                .flat_map(|observation| observation.assessments.iter()),
        )
    }) {
        reasons
            .entry(pressure_reason_code(assessment))
            .or_insert_with(|| assessment.recommendation.clone());
    }
    if pressure.iter().any(|pressure| {
        pressure
            .pressure_freshness
            .is_some_and(|freshness| !freshness.fresh)
            || pressure
                .subscriber_observations
                .iter()
                .any(|observation| !observation.fresh)
    }) {
        reasons.insert(
            DiagnosticReasonCode::MetricsStale,
            Some("one or more pressure snapshots are outside the freshness window".to_owned()),
        );
    }
    reasons
        .into_iter()
        .map(|(code, detail)| WhyReasonOutput { code, detail })
        .collect()
}

fn why_suggestions(reasons: &[WhyReasonOutput], component: &ComponentInspection) -> Vec<String> {
    let codes = reasons
        .iter()
        .map(|reason| reason.code)
        .collect::<BTreeSet<_>>();
    let mut suggestions = Vec::new();
    if codes.contains(&DiagnosticReasonCode::RequirementUnresolved) {
        suggestions
            .push("inspect active provider candidates for each unresolved requirement".to_owned());
    }
    if codes.contains(&DiagnosticReasonCode::ProviderDisappeared) {
        suggestions.push(
            "inspect the retained provider binding and that provider's current state".to_owned(),
        );
    }
    if codes.contains(&DiagnosticReasonCode::ActivationFailed)
        || codes.contains(&DiagnosticReasonCode::CriticalTaskFailed)
        || codes.contains(&DiagnosticReasonCode::RuntimeReported)
    {
        suggestions.push(
            "inspect the runtime-reported state detail and pending Composer operation".to_owned(),
        );
    }
    if codes.iter().any(|code| {
        matches!(
            code,
            DiagnosticReasonCode::PressureLoanCapacity
                | DiagnosticReasonCode::PressureBorrowCapacity
                | DiagnosticReasonCode::PressureControlDelivery
        )
    }) {
        suggestions.push("inspect pressure capacity counters and their freshness before changing lifecycle state".to_owned());
    }
    if codes.contains(&DiagnosticReasonCode::MetricsStale) {
        suggestions.push("refresh inspection before acting on stale pressure metrics".to_owned());
    }
    if suggestions.is_empty() && matches!(component.snapshot.state, ComponentState::Active) {
        suggestions.push("component is active; no lifecycle blocker is reported".to_owned());
    }
    suggestions
}

fn why_projection(view: &InspectionView, component_id: &str) -> Result<WhyOutput, CliError> {
    let component = view
        .components
        .iter()
        .find(|component| component.component_id.as_ref() == component_id)
        .ok_or_else(|| CliError::NotFound(format!("component {component_id:?} is not known")))?;
    let mut requirement_values = component
        .snapshot
        .requirements
        .iter()
        .map(|requirement| requirement_output(view, component, requirement))
        .collect::<Vec<_>>();
    requirement_values.sort_by(|left, right| {
        left.requirement_name
            .cmp(&right.requirement_name)
            .then_with(|| left.capability_type.cmp(&right.capability_type))
    });
    let pressure_values = relevant_pressure(view, component)
        .into_iter()
        .filter_map(why_pressure)
        .collect::<Vec<_>>();
    let reasons = why_reasons(view, component, &requirement_values, &pressure_values);
    let reason_codes = reasons.iter().map(|reason| reason.code).collect();
    Ok(WhyOutput {
        component: component.component_id.to_string(),
        component_state: state_name(&component.snapshot.state).to_ascii_lowercase(),
        state_detail: state_detail(&component.snapshot.state),
        control_connection: connection_name(component).to_owned(),
        snapshot_received: component.snapshot_received,
        pending: component.pending.clone(),
        reasons: reasons.clone(),
        reason_codes,
        requirements: requirement_values,
        pressure_assessments: pressure_values,
        suggestions: why_suggestions(&reasons, component),
        freshness: projection_freshness(view, Some(component)),
        last_active: component.last_active.clone(),
        process_conflict: view
            .process_conflicts
            .iter()
            .find(|conflict| conflict.component_id.as_ref() == component_id)
            .cloned(),
    })
}

/// Render a typed why explanation as JSON.
pub fn format_why_json(view: &InspectionView, component_id: &str) -> Result<String, CliError> {
    format_json(&why_projection(view, component_id)?)
}

fn display_why_id(value: &str, full: bool) -> String {
    if full {
        return value.to_owned();
    }
    Uuid::parse_str(value).map_or_else(|_| value.to_owned(), |uuid| display_uuid(uuid, false))
}

fn display_why_process(value: rbt_core::ProcessInstanceId, full: bool) -> String {
    display_uuid(value.into_uuid(), full)
}

/// Render why a component is blocked, failed, or active with optional full IDs.
pub fn format_why_with_options(
    view: &InspectionView,
    component_id: &str,
    full: bool,
) -> Result<String, CliError> {
    let why = why_projection(view, component_id)?;
    let mut output = String::new();
    let _ = writeln!(
        output,
        "{} is {}",
        why.component,
        why.component_state.to_ascii_uppercase()
    );
    let _ = writeln!(output, "control: {}", why.control_connection);
    let _ = writeln!(
        output,
        "snapshot: {} (lifecycle age: {})",
        if why.snapshot_received {
            "received from live Composer response"
        } else {
            "not received from live Composer response"
        },
        lifecycle_age_label_for_freshness(&why.freshness.lifecycle_freshness),
    );
    if let Some(pending) = &why.pending {
        let _ = writeln!(
            output,
            "\npending:\n  {}: {}",
            pending.operation, pending.reason
        );
    }
    if why.reasons.is_empty() {
        output.push_str("\nreasons: none reported\n");
    } else {
        output.push_str("\nreasons:\n");
        for reason in &why.reasons {
            let _ = writeln!(
                output,
                "  - {}{}",
                serde_json::to_string(&reason.code)
                    .unwrap_or_else(|_| "runtime_reported".to_owned())
                    .trim_matches('"'),
                reason
                    .detail
                    .as_ref()
                    .map_or_else(String::new, |detail| format!(": {detail}"))
            );
        }
    }
    if why.requirements.is_empty() {
        output.push_str("\nrequirements: none\n");
    } else {
        output.push_str("\nrequirements:\n");
        for requirement in &why.requirements {
            let _ = writeln!(
                output,
                "  - {} ({}) — {}{}",
                requirement.requirement_name,
                requirement.capability_type,
                if requirement.resolved {
                    "resolved"
                } else {
                    "missing/unresolved"
                },
                requirement
                    .binding
                    .as_ref()
                    .map_or_else(String::new, |binding| {
                        format!(
                            "; provider {} capability {}",
                            binding.provider,
                            display_why_id(&binding.capability_instance, full)
                        )
                    })
            );
            if !requirement.resolved
                && requirement.transport_kind == Some(CapabilityTransportKind::Rpc)
            {
                if let (Some(request), Some(response)) =
                    (&requirement.request_type, &requirement.response_type)
                {
                    let _ = writeln!(
                        output,
                        "    Transport: RPC<{}, {}>",
                        request.qualified_name(),
                        response.qualified_name()
                    );
                } else {
                    output.push_str("    Transport: RPC<request, response>\n");
                }
            }
            if !requirement.candidates.is_empty() {
                for candidate in &requirement.candidates {
                    let failure_reason = candidate
                        .failure_reason
                        .as_ref()
                        .map_or_else(String::new, |reason| format!(" reason={reason}"));
                    let _ = writeln!(
                        output,
                        "    candidate provider={} capability={} transport={} state={}{}",
                        candidate.provider,
                        display_why_id(&candidate.capability_instance, full),
                        transport_label(Some(candidate.transport_kind)),
                        candidate.state,
                        failure_reason,
                    );
                }
            } else if !requirement.resolved {
                output.push_str("    no active provider\n");
            }
        }
    }
    if !why.pressure_assessments.is_empty() {
        output.push_str("\npressure:\n");
        for pressure in &why.pressure_assessments {
            let freshness = pressure.pressure_freshness.map_or_else(
                || "unavailable".to_owned(),
                |freshness| {
                    format!(
                        "{} (age_ms={})",
                        if freshness.fresh { "fresh" } else { "stale" },
                        freshness.age_ms
                    )
                },
            );
            let _ = writeln!(
                output,
                "  - {} {} provider={} pressure={} assessments={}",
                pressure.capability_type,
                display_why_id(&pressure.capability_instance, full),
                pressure.provider,
                if pressure.pressure_available {
                    freshness
                } else {
                    "unavailable".to_owned()
                },
                pressure.assessments.len()
            );
        }
    }
    if !why.suggestions.is_empty() {
        output.push_str("\nsuggestions:\n");
        for suggestion in &why.suggestions {
            let _ = writeln!(output, "  - {suggestion}");
        }
    }
    if let Some(last_active) = &why.last_active {
        let _ = writeln!(
            output,
            "\nlast active:\n  process: {}\n  activation: {}",
            display_why_process(last_active.process_instance, full),
            display_why_id(&last_active.activation_id.to_string(), full),
        );
        for binding in &last_active.bindings {
            let _ = writeln!(
                output,
                "  provider for {} ({}): {}\n    capability: {}",
                binding.requirement_name,
                binding.capability_type,
                binding.provider,
                display_why_id(&binding.descriptor.capability_instance.to_string(), full)
            );
        }
    }
    if let Some(conflict) = &why.process_conflict {
        let _ = writeln!(
            output,
            "\nprocess conflict:\n  incumbent: {}\n  contender: {}",
            display_why_process(conflict.incumbent, full),
            display_why_process(conflict.contender, full),
        );
    }
    Ok(output)
}

/// Render why a component with compact human-readable IDs.
pub fn format_why(view: &InspectionView, component_id: &str) -> Result<String, CliError> {
    format_why_with_options(view, component_id, false)
}

/// Return active capabilities filtered by logical capability type and/or
/// provider component.  Matching is exact because both fields are stable
/// control-plane identities, not human-formatted strings.
pub fn filter_capabilities<'a>(
    view: &'a InspectionView,
    capability_type: Option<&str>,
    provider: Option<&str>,
) -> Vec<&'a CapabilityInspection> {
    let mut capabilities = view
        .capability_topology
        .iter()
        .filter(|capability| {
            capability_type.is_none_or(|value| capability.capability_type == value)
                && provider.is_none_or(|value| capability.provider_component.as_ref() == value)
        })
        .collect::<Vec<_>>();
    capabilities.sort_by(|left, right| {
        left.capability_type
            .cmp(&right.capability_type)
            .then_with(|| left.provider_component.cmp(&right.provider_component))
            .then_with(|| left.capability_instance.cmp(&right.capability_instance))
            .then_with(|| left.physical_endpoint.cmp(&right.physical_endpoint))
    });
    capabilities
}

/// Alias with an explicit topology name for callers that prefer it.
pub fn filter_capability_topology<'a>(
    view: &'a InspectionView,
    capability_type: Option<&str>,
    provider: Option<&str>,
) -> Vec<&'a CapabilityInspection> {
    filter_capabilities(view, capability_type, provider)
}

fn format_capability_list_inner(
    view: &InspectionView,
    capability_type: Option<&str>,
    provider: Option<&str>,
    verbose: bool,
) -> String {
    let capabilities = filter_capabilities(view, capability_type, provider);
    let mut output = if verbose {
        String::from(
            "TYPE\tNAME\tPROVIDER\tCAPABILITY_INSTANCE\tTRANSPORT\tENDPOINT\tMESSAGE\tREQUEST\tRESPONSE\tPROFILE\n",
        )
    } else {
        String::from(
            "TYPE\tNAME\tPROVIDER\tCAPABILITY_INSTANCE\tTRANSPORT\tMESSAGE\tREQUEST\tRESPONSE\tPROFILE\n",
        )
    };
    for capability in &capabilities {
        let (transport, message, request, response, profile) = match &capability.transport {
            CapabilityTransportInspection::Stream {
                message_type,
                profile,
                ..
            } => (
                "Stream",
                message_type.qualified_name(),
                "-".to_owned(),
                "-".to_owned(),
                profile_name(*profile).to_owned(),
            ),
            CapabilityTransportInspection::Rpc {
                request_type,
                response_type,
                ..
            } => (
                "RPC",
                "-".to_owned(),
                descriptor_summary(request_type),
                descriptor_summary(response_type),
                "-".to_owned(),
            ),
        };
        if verbose {
            let _ = writeln!(
                output,
                "{}\t-\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                capability.capability_type,
                capability.provider_component,
                capability.capability_instance,
                capability.physical_endpoint,
                transport,
                message,
                request,
                response,
                profile,
            );
        } else {
            let _ = writeln!(
                output,
                "{}\t-\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                capability.capability_type,
                capability.provider_component,
                display_uuid(capability.capability_instance, false),
                transport,
                message,
                request,
                response,
                profile,
            );
        }
    }
    if capabilities.is_empty() {
        output.push_str("(no active capabilities)\n");
    }
    output
}

fn descriptor_summary(descriptor: &rbt_core::MessageTypeDescriptor) -> String {
    format!(
        "{} [{}]",
        descriptor.qualified_name(),
        descriptor.layout_hash
    )
}

/// Render active capabilities with logical fields only.  Physical endpoints
/// and full UUIDs are intentionally hidden in the default human view.
pub fn format_capability_list(view: &InspectionView) -> String {
    format_capability_list_inner(view, None, None, false)
}

/// Render active capabilities with full UUIDs and physical endpoints.
pub fn format_capability_list_verbose(view: &InspectionView) -> String {
    format_capability_list_inner(view, None, None, true)
}

/// Render a filtered logical capability list.
pub fn format_capability_list_filtered(
    view: &InspectionView,
    capability_type: Option<&str>,
    provider: Option<&str>,
) -> String {
    format_capability_list_inner(view, capability_type, provider, false)
}

/// Render a filtered capability list retaining full endpoint identity.
pub fn format_capability_list_filtered_verbose(
    view: &InspectionView,
    capability_type: Option<&str>,
    provider: Option<&str>,
) -> String {
    format_capability_list_inner(view, capability_type, provider, true)
}

/// Render a filtered machine-readable capability list. The underlying
/// inspection records retain full UUIDs and physical endpoints.
pub fn format_capability_list_json(
    view: &InspectionView,
    capability_type: Option<&str>,
    provider: Option<&str>,
) -> Result<String, CliError> {
    let mut capabilities = view
        .capability_topology
        .iter()
        .filter(|capability| {
            capability_type.is_none_or(|expected| capability.capability_type == expected)
                && provider
                    .is_none_or(|expected| capability.provider_component.as_ref() == expected)
        })
        .cloned()
        .collect::<Vec<_>>();
    capabilities.sort_by(|left, right| {
        left.capability_type
            .cmp(&right.capability_type)
            .then_with(|| left.provider_component.cmp(&right.provider_component))
            .then_with(|| left.capability_instance.cmp(&right.capability_instance))
    });
    format_json(&capabilities)
}

/// Structured stream metadata used by `rbt stream info --json`.
#[derive(Clone, Debug, Serialize)]
pub struct StreamInfoOutput {
    pub capability_type: String,
    pub capability_instance: rbt_core::CapabilityInstanceId,
    pub physical_endpoint: String,
    pub provider_component: rbt_core::ComponentId,
    pub provider_process: rbt_core::ProcessInstanceId,
    pub activation: rbt_core::ActivationId,
    pub message_type: rbt_core::MessageTypeDescriptor,
    pub profile: rbt_core::StreamProfile,
    pub readiness: rbt_core::ReadinessProtocol,
    pub semantic: StreamSemanticOutput,
    pub resources: rbt_core::StreamResourceConfig,
    pub pressure: Option<rbt_core::StreamPressure>,
    pub pressure_freshness: Option<PressureFreshness>,
    pub subscriber_observations: Vec<SubscriberPressureObservation>,
    pub assessments: Vec<PressureAssessment>,
}

/// Structured frozen stream semantics used by the stream-info output.
#[derive(Clone, Debug, Serialize)]
pub struct StreamSemanticOutput {
    pub queue_depth: usize,
    pub history_size: Option<usize>,
    pub safe_overflow: bool,
}

/// Structured stream pressure used by `rbt stream pressure --json`.
#[derive(Clone, Debug, Serialize)]
pub struct StreamPressureOutput {
    pub capability_type: String,
    pub capability_instance: rbt_core::CapabilityInstanceId,
    pub physical_endpoint: String,
    pub provider_component: rbt_core::ComponentId,
    pub provider_process: rbt_core::ProcessInstanceId,
    pub activation: rbt_core::ActivationId,
    pub profile: rbt_core::StreamProfile,
    pub semantic: StreamSemanticOutput,
    pub resources: rbt_core::StreamResourceConfig,
    pub snapshot: Option<rbt_core::StreamPressure>,
    pub snapshot_freshness: Option<PressureFreshness>,
    pub publisher: Option<rbt_core::PublisherPressure>,
    pub subscriber_observations: Vec<SubscriberPressureObservation>,
    pub assessments: Vec<PressureAssessment>,
}

/// Structured RPC metadata and low-frequency metrics used by
/// `rbt rpc info --json`.
#[derive(Clone, Debug, Serialize)]
pub struct RpcInfoOutput {
    pub transport_kind: CapabilityTransportKind,
    pub capability_type: String,
    pub capability_instance: rbt_core::CapabilityInstanceId,
    pub physical_endpoint: String,
    pub provider_component: rbt_core::ComponentId,
    pub provider_process: rbt_core::ProcessInstanceId,
    pub activation: rbt_core::ActivationId,
    pub request_type: rbt_core::MessageTypeDescriptor,
    pub response_type: rbt_core::MessageTypeDescriptor,
    pub readiness: rbt_core::RpcReadinessProtocol,
    pub resources: rbt_core::RpcResourceConfig,
    pub server_pressure: Option<rbt_core::RpcPressure>,
    pub server_pressure_freshness: Option<PressureFreshness>,
    pub client_observations: Vec<RpcClientPressureObservation>,
    pub metrics_status: String,
}

fn rpc_info_output(capability: &CapabilityInspection) -> Result<RpcInfoOutput, CliError> {
    let CapabilityTransportInspection::Rpc {
        request_type,
        response_type,
        readiness,
        resources,
        server_pressure,
        server_pressure_freshness,
        client_observations,
    } = &capability.transport
    else {
        return Err(CliError::Usage(format!(
            "capability {:?} is a Stream endpoint; use `rbt stream info {}`",
            capability.physical_endpoint, capability.physical_endpoint
        )));
    };
    Ok(RpcInfoOutput {
        transport_kind: CapabilityTransportKind::Rpc,
        capability_type: capability.capability_type.clone(),
        capability_instance: capability.capability_instance,
        physical_endpoint: capability.physical_endpoint.clone(),
        provider_component: capability.provider_component.clone(),
        provider_process: capability.provider_process,
        activation: capability.activation,
        request_type: request_type.clone(),
        response_type: response_type.clone(),
        readiness: *readiness,
        resources: *resources,
        server_pressure: server_pressure.clone(),
        server_pressure_freshness: *server_pressure_freshness,
        client_observations: client_observations.clone(),
        metrics_status: rpc_metrics_status(
            server_pressure.as_ref(),
            server_pressure_freshness.as_ref(),
            client_observations,
        ),
    })
}

/// Render a typed RPC-info JSON response.
pub fn format_rpc_info_json(view: &InspectionView, query: &str) -> Result<String, CliError> {
    format_json(&rpc_info_output(find_capability(view, query)?)?)
}

/// Render request/response ABI and bounded RPC transport metadata.
pub fn format_rpc_info(
    view: &InspectionView,
    query: &str,
    verbose: bool,
) -> Result<String, CliError> {
    let info = rpc_info_output(find_capability(view, query)?)?;
    let mut output = String::new();
    let _ = writeln!(output, "semantic type: {}", info.capability_type);
    let _ = writeln!(output, "transport: RPC");
    let _ = writeln!(
        output,
        "capability instance: {}",
        display_uuid(info.capability_instance, verbose)
    );
    if verbose {
        let _ = writeln!(output, "physical endpoint: {}", info.physical_endpoint);
    }
    let _ = writeln!(output, "provider: {}", info.provider_component);
    let _ = writeln!(
        output,
        "request ABI: {}",
        info.request_type.qualified_name()
    );
    let _ = writeln!(
        output,
        "request layout hash: {}",
        info.request_type.layout_hash
    );
    let _ = writeln!(
        output,
        "response ABI: {}",
        info.response_type.qualified_name()
    );
    let _ = writeln!(
        output,
        "response layout hash: {}",
        info.response_type.layout_hash
    );
    let _ = writeln!(output, "readiness: {}", info.readiness);
    output.push_str("resources:\n");
    let resources = info.resources;
    let _ = writeln!(output, "  max in flight: {}", resources.max_in_flight);
    let _ = writeln!(output, "  max clients: {}", resources.max_clients);
    let _ = writeln!(
        output,
        "  max request loans: {}",
        resources.max_request_loans
    );
    let _ = writeln!(
        output,
        "  response buffer size: {}",
        resources.response_buffer_size
    );
    let _ = writeln!(
        output,
        "  max borrowed responses: {}",
        resources.max_borrowed_responses
    );
    let _ = writeln!(
        output,
        "  max response loans: {}",
        resources.max_response_loans
    );
    let _ = writeln!(
        output,
        "  max request slice length: {}",
        resources.max_request_slice_len
    );
    let _ = writeln!(
        output,
        "  max response slice length: {}",
        resources.max_response_slice_len
    );
    let _ = writeln!(output, "metrics: {}", info.metrics_status);
    match (&info.server_pressure, info.server_pressure_freshness) {
        (Some(pressure), freshness) => {
            let label = freshness.map_or(
                "unknown",
                |value| {
                    if value.fresh { "fresh" } else { "stale" }
                },
            );
            if let Some(metrics) = pressure.server {
                let _ = writeln!(
                    output,
                    "  server ({label}): requests received={} current={} high-water={} responses sent={} failed={} abandoned={}",
                    metrics.requests_received,
                    metrics.requests_current,
                    metrics.requests_high_water,
                    metrics.responses_sent,
                    metrics.responses_failed,
                    metrics.responses_abandoned
                );
            }
        }
        (None, _) => output.push_str("  server: unavailable\n"),
    }
    if info.client_observations.is_empty() {
        output.push_str("  clients: unavailable\n");
    } else {
        let _ = writeln!(
            output,
            "  clients: {} current observation(s)",
            info.client_observations.len()
        );
        for observation in &info.client_observations {
            let Some(metrics) = observation.pressure.client else {
                continue;
            };
            let _ = writeln!(
                output,
                "    {} ({}): attempted={} completed={} failed={} timed-out={} cancelled={} in-flight={} high-water={}",
                observation.component_id,
                if observation.fresh { "fresh" } else { "stale" },
                metrics.calls_attempted,
                metrics.calls_completed,
                metrics.calls_failed,
                metrics.calls_timed_out,
                metrics.calls_cancelled,
                metrics.in_flight_current,
                metrics.in_flight_high_water
            );
        }
    }
    Ok(output)
}

fn rpc_metrics_status(
    server: Option<&rbt_core::RpcPressure>,
    server_freshness: Option<&PressureFreshness>,
    clients: &[RpcClientPressureObservation],
) -> String {
    if server.is_none() && clients.is_empty() {
        return "unavailable".to_owned();
    }
    if server_freshness.is_some_and(|value| value.fresh)
        || clients.iter().any(|observation| observation.fresh)
    {
        "available".to_owned()
    } else {
        "stale".to_owned()
    }
}

fn stream_only_error(query: &str) -> CliError {
    CliError::Usage(format!(
        "capability {query:?} is an RPC endpoint; use `rbt rpc info {query}`"
    ))
}

fn stream_info_output(capability: &CapabilityInspection) -> Result<StreamInfoOutput, CliError> {
    let CapabilityTransportInspection::Stream {
        message_type,
        profile,
        readiness,
        queue_depth,
        history_size,
        safe_overflow,
        resources,
        pressure,
        pressure_freshness,
        subscriber_observations,
        assessments,
    } = &capability.transport
    else {
        return Err(stream_only_error(&capability.physical_endpoint));
    };
    Ok(StreamInfoOutput {
        capability_type: capability.capability_type.clone(),
        capability_instance: capability.capability_instance,
        physical_endpoint: capability.physical_endpoint.clone(),
        provider_component: capability.provider_component.clone(),
        provider_process: capability.provider_process,
        activation: capability.activation,
        message_type: message_type.clone(),
        profile: *profile,
        readiness: *readiness,
        semantic: StreamSemanticOutput {
            queue_depth: *queue_depth,
            history_size: *history_size,
            safe_overflow: *safe_overflow,
        },
        resources: *resources,
        pressure: pressure.clone(),
        pressure_freshness: *pressure_freshness,
        subscriber_observations: subscriber_observations.clone(),
        assessments: assessments.clone(),
    })
}

fn stream_pressure_output(
    capability: &CapabilityInspection,
) -> Result<StreamPressureOutput, CliError> {
    let CapabilityTransportInspection::Stream {
        profile,
        queue_depth,
        history_size,
        safe_overflow,
        resources,
        pressure,
        pressure_freshness,
        subscriber_observations,
        assessments,
        ..
    } = &capability.transport
    else {
        return Err(stream_only_error(&capability.physical_endpoint));
    };
    Ok(StreamPressureOutput {
        capability_type: capability.capability_type.clone(),
        capability_instance: capability.capability_instance,
        physical_endpoint: capability.physical_endpoint.clone(),
        provider_component: capability.provider_component.clone(),
        provider_process: capability.provider_process,
        activation: capability.activation,
        profile: *profile,
        semantic: StreamSemanticOutput {
            queue_depth: *queue_depth,
            history_size: *history_size,
            safe_overflow: *safe_overflow,
        },
        resources: *resources,
        snapshot: pressure.clone(),
        snapshot_freshness: *pressure_freshness,
        publisher: pressure.as_ref().and_then(|pressure| pressure.publisher),
        subscriber_observations: subscriber_observations.clone(),
        assessments: assessments.clone(),
    })
}

/// Render a typed stream-info JSON response.
pub fn format_stream_info_json(view: &InspectionView, query: &str) -> Result<String, CliError> {
    format_json(&stream_info_output(find_capability(view, query)?)?)
}

/// Render a typed stream-pressure JSON response.
pub fn format_stream_pressure_json(view: &InspectionView, query: &str) -> Result<String, CliError> {
    format_json(&stream_pressure_output(find_capability(view, query)?)?)
}

/// Render a stream's ABI and profile details.
pub fn format_stream_info(view: &InspectionView, query: &str) -> Result<String, CliError> {
    format_stream_info_inner(view, query, false)
}

/// Render a stream's ABI and profile details with full identity metadata.
///
/// This is the explicit diagnostic view for operators who need to correlate a
/// logical capability with its physical transport endpoint.
pub fn format_stream_info_verbose(view: &InspectionView, query: &str) -> Result<String, CliError> {
    format_stream_info_inner(view, query, true)
}

fn format_stream_info_inner(
    view: &InspectionView,
    query: &str,
    verbose: bool,
) -> Result<String, CliError> {
    let capability = find_capability(view, query)?;
    let CapabilityTransportInspection::Stream {
        message_type,
        profile,
        readiness,
        queue_depth,
        history_size,
        safe_overflow,
        resources,
        pressure,
        pressure_freshness,
        assessments,
        ..
    } = &capability.transport
    else {
        return Err(stream_only_error(query));
    };
    let message = message_type;
    let mut output = String::new();
    let _ = writeln!(output, "semantic type: {}", capability.capability_type);
    let _ = writeln!(
        output,
        "capability instance: {}",
        display_uuid(capability.capability_instance, verbose)
    );
    if verbose {
        let _ = writeln!(
            output,
            "physical endpoint: {}",
            capability.physical_endpoint
        );
    }
    let _ = writeln!(output, "provider: {}", capability.provider_component);
    output.push_str("connections:\n");
    output.push_str("  publishers: 1\n");
    let _ = writeln!(
        output,
        "  subscribers: {}",
        pressure
            .as_ref()
            .and_then(|pressure| pressure.publisher)
            .map_or_else(
                || "unknown".to_owned(),
                |publisher| publisher.subscriber_count.to_string()
            )
    );
    let _ = writeln!(output, "message ABI: {}", message.qualified_name());
    let _ = writeln!(output, "layout hash: {}", message.layout_hash);
    let _ = writeln!(output, "shape: {:?}", message.shape);
    let _ = writeln!(output, "profile: {:?}", profile);
    let _ = writeln!(output, "readiness: {}", readiness.as_str());
    let _ = writeln!(
        output,
        "header size/alignment: {}/{}",
        message.header_size, message.header_align
    );
    let _ = writeln!(
        output,
        "element size/alignment: {}/{}",
        message.element_size, message.element_align
    );
    output.push_str("semantics:\n");
    let _ = writeln!(output, "  queue depth: {}", queue_depth);
    let _ = writeln!(
        output,
        "  history: {}",
        history_size.map_or_else(|| "unknown".to_owned(), |value| value.to_string())
    );
    let _ = writeln!(output, "  safe overflow: {}", safe_overflow);
    output.push_str("resources:\n");
    let _ = writeln!(output, "  max loans: {}", resources.max_loans);
    let _ = writeln!(output, "  max borrowed: {}", resources.max_borrowed);
    let _ = writeln!(
        output,
        "resource config source: {}",
        if pressure.is_some() {
            "descriptor/provider transport report"
        } else {
            "capability descriptor (provider pressure unavailable)"
        }
    );
    match pressure_freshness {
        Some(freshness) => {
            let _ = writeln!(output, "pressure snapshot age: {} ms", freshness.age_ms);
            let _ = writeln!(
                output,
                "pressure snapshot status: {}",
                if freshness.fresh { "fresh" } else { "stale" }
            );
        }
        None => output.push_str("pressure snapshot: unavailable\n"),
    }
    if assessments.is_empty() {
        output.push_str(if pressure.is_some() {
            "pressure summary: no current condition observed\n"
        } else {
            "pressure summary: metrics unavailable\n"
        });
    } else {
        output.push_str("pressure summary:\n");
        for assessment in assessments {
            let _ = writeln!(output, "  - {}", assessment_reason_name(assessment));
        }
    }
    Ok(output)
}

/// Render a stream's latest low-rate, provider-reported pressure snapshot.
pub fn format_stream_pressure(view: &InspectionView, query: &str) -> Result<String, CliError> {
    let capability = find_capability(view, query)?;
    let CapabilityTransportInspection::Stream {
        profile,
        message_type,
        queue_depth,
        history_size,
        safe_overflow,
        resources,
        pressure,
        pressure_freshness,
        subscriber_observations,
        assessments,
        ..
    } = &capability.transport
    else {
        return Err(stream_only_error(query));
    };
    let mut output = String::new();
    output.push_str("stream:\n");
    let _ = writeln!(output, "  capability type: {}", capability.capability_type);
    let _ = writeln!(
        output,
        "  capability instance: {}",
        display_uuid(capability.capability_instance, false)
    );
    let _ = writeln!(output, "  profile: {:?}", profile);
    let _ = writeln!(output, "  message: {}", message_type.qualified_name());
    output.push_str("semantics:\n");
    let _ = writeln!(output, "  queue depth: {}", queue_depth);
    let _ = writeln!(
        output,
        "  history: {}",
        history_size.map_or_else(|| "unknown".to_owned(), |value| value.to_string())
    );
    let _ = writeln!(output, "  safe overflow: {}", safe_overflow);
    output.push_str("resources:\n");
    let _ = writeln!(output, "  max loans: {}", resources.max_loans);
    let _ = writeln!(output, "  max borrowed: {}", resources.max_borrowed);

    output.push_str("publisher:\n");
    let Some(pressure) = pressure else {
        output.push_str(
            "  source: unavailable (provider has not reported a live rbt stream wrapper)\n",
        );
        output.push_str("  sent: unknown\n  send_failures: unknown\n  loans_in_use: unknown\n  loan_failures: unknown\n");
        output.push_str("  snapshot_age_ms: unknown\n  snapshot_status: unavailable\n");
        format_subscriber_text(&mut output, subscriber_observations, None);
        output.push_str("pressure:\n  estimated_dropped: unknown\n  last_event: unknown\n");
        return Ok(output);
    };

    let Some(publisher) = pressure.publisher else {
        output.push_str("  source: unavailable (matching report was not publisher-side)\n");
        output.push_str("  sent: unknown\n  send_failures: unknown\n  loans_in_use: unknown\n  loan_failures: unknown\n");
        output.push_str("  snapshot_age_ms: unknown\n  snapshot_status: unavailable\n");
        format_subscriber_text(&mut output, subscriber_observations, Some(pressure));
        output.push_str("pressure:\n  estimated_dropped: unknown\n  last_event: unknown\n");
        return Ok(output);
    };

    output.push_str("  source: transport observed, per capability instance\n");
    if let Some(freshness) = pressure_freshness {
        let _ = writeln!(output, "  snapshot_age_ms: {}", freshness.age_ms);
        let _ = writeln!(
            output,
            "  snapshot_status: {}",
            if freshness.fresh { "fresh" } else { "stale" }
        );
    } else {
        output.push_str("  snapshot_age_ms: unknown\n  snapshot_status: unavailable\n");
    }
    let _ = writeln!(output, "  sent: {}", publisher.send_successes);
    let _ = writeln!(output, "  send_attempts: {}", publisher.send_attempts);
    let _ = writeln!(output, "  send_failures: {}", publisher.send_failures);
    let _ = writeln!(output, "  loans_in_use: {}", publisher.loans_in_use);
    let _ = writeln!(output, "  loan_attempts: {}", publisher.loan_attempts);
    let _ = writeln!(output, "  loan_successes: {}", publisher.loan_successes);
    let _ = writeln!(output, "  loans_high_water: {}", publisher.loans_high_water);
    let _ = writeln!(output, "  loan_failures: {}", publisher.loan_failures);
    let _ = writeln!(
        output,
        "  payload_bytes_last: {}",
        publisher.payload_bytes_last
    );
    let _ = writeln!(
        output,
        "  payload_bytes_max_seen: {}",
        publisher.payload_bytes_max_seen
    );
    let _ = writeln!(output, "  total_loaned: {}", publisher.total_loaned);
    let _ = writeln!(output, "  queue_full: {}", publisher.queue_full);
    let _ = writeln!(
        output,
        "  unable_to_deliver: {}",
        publisher.unable_to_deliver
    );
    let _ = writeln!(
        output,
        "  readiness_notify_failures: {}",
        publisher.readiness_notify_failures
    );
    format_subscriber_text(&mut output, subscriber_observations, Some(pressure));
    output.push_str("pressure:\n");
    match publisher.estimated_dropped_samples {
        Some(dropped) => {
            let _ = writeln!(output, "  estimated_dropped: {dropped}");
        }
        None => output.push_str("  estimated_dropped: unknown\n"),
    }
    match &pressure.last_pressure_event {
        Some(event) => {
            let _ = writeln!(output, "  last_event: {event:?}");
        }
        None => output.push_str("  last_event: none observed\n"),
    }
    if !assessments.is_empty() {
        output.push_str("assessments:\n");
        for assessment in assessments {
            let _ = writeln!(
                output,
                "  - {}{}",
                assessment_reason_name(assessment),
                assessment
                    .recommendation
                    .as_ref()
                    .map_or_else(String::new, |recommendation| format!(": {recommendation}"))
            );
            if let Some(bytes) = assessment.approximate_payload_only_bytes {
                let _ = writeln!(
                    output,
                    "    approximate payload-only bytes for one extra loan: {bytes}"
                );
            }
        }
    }
    Ok(output)
}

fn format_subscriber_text(
    output: &mut String,
    subscriber_observations: &[SubscriberPressureObservation],
    publisher: Option<&rbt_core::StreamPressure>,
) {
    output.push_str("subscribers:\n");
    if let Some(publisher) = publisher.and_then(|pressure| pressure.publisher) {
        let _ = writeln!(output, "  connected: {}", publisher.subscriber_count);
    } else {
        output.push_str("  connected: unknown\n");
    }
    if subscriber_observations.is_empty() {
        output.push_str("  observations: unavailable\n");
    } else {
        output.push_str("  observations:\n");
        for observation in subscriber_observations {
            let _ = writeln!(
                output,
                "    - component={} process={} activation={} age_ms={} status={}",
                observation.component_id,
                observation.process_instance_id,
                observation.activation_id.map_or_else(
                    || "inactive".to_owned(),
                    |activation| activation.to_string()
                ),
                observation.age_ms,
                if observation.fresh { "fresh" } else { "stale" },
            );
            if let Some(subscriber) = observation.pressure.subscriber {
                let _ = writeln!(
                    output,
                    "      receive_attempts={} received_samples={} borrowed_samples={} borrowed_high_water={} borrow_failures={} borrow_duration_count={} borrow_duration_total_ns={} borrow_duration_max_ns={} oldest_borrow_age_ns={} estimated_dropped_samples={}",
                    subscriber.receive_attempts,
                    subscriber.received_samples,
                    subscriber.borrowed_samples,
                    subscriber.borrowed_high_water,
                    subscriber.borrow_failures,
                    subscriber.borrow_duration_count,
                    subscriber.borrow_duration_total_ns,
                    subscriber.borrow_duration_max_ns,
                    subscriber
                        .oldest_borrow_age_ns
                        .map_or_else(|| "none".to_owned(), |value| value.to_string()),
                    subscriber
                        .estimated_dropped_samples
                        .map_or_else(|| "unknown".to_owned(), |value| value.to_string()),
                );
            }
            for assessment in &observation.assessments {
                let _ = writeln!(
                    output,
                    "      assessment: {}",
                    assessment_reason_name(assessment)
                );
            }
        }
    }
    if publisher.is_some() && !subscriber_observations.is_empty() {
        output.push_str("  note: queue_full does not imply that nobody received a sample\n");
    }
}

fn assessment_reason_name(assessment: &PressureAssessment) -> &'static str {
    match assessment.reason {
        rbt_compose::PressureAssessmentReason::LoanCapacityReached => "loan_capacity_reached",
        rbt_compose::PressureAssessmentReason::BorrowCapacityReached => "borrow_capacity_reached",
        rbt_compose::PressureAssessmentReason::OutstandingBorrow => "outstanding_borrow",
        rbt_compose::PressureAssessmentReason::ControlDeliveryPressure => {
            "control_delivery_pressure"
        }
        rbt_compose::PressureAssessmentReason::ReadinessFailuresObserved => {
            "readiness_failures_observed"
        }
    }
}

/// Render a machine-readable capability list or stream info response.
pub fn format_json<T: Serialize>(value: &T) -> Result<String, CliError> {
    serde_json::to_string_pretty(value).map_err(Into::into)
}

/// Render concise MCAP information without consulting Composer.
pub fn format_mcap_info(info: &rbt_mcap::RecordingInfo) -> String {
    let mut output = String::new();
    let _ = writeln!(output, "file: {}", info.path.display());
    let _ = writeln!(output, "size: {} bytes", info.size_bytes);
    let _ = writeln!(output, "profile: {}", info.profile);
    let _ = writeln!(output, "library: {}", info.library);
    let _ = writeln!(output, "messages: {}", info.message_count);
    let _ = writeln!(output, "duration: {} ns", info.duration_ns);
    let _ = writeln!(
        output,
        "time range: {} .. {}",
        info.first_log_time
            .map_or_else(|| "-".to_owned(), |value| value.to_string()),
        info.last_log_time
            .map_or_else(|| "-".to_owned(), |value| value.to_string())
    );
    output.push_str("schemas:\n");
    for schema in &info.schemas {
        let _ = writeln!(
            output,
            "  {} id={} encoding={} bytes={}",
            schema.name, schema.id, schema.encoding, schema.data_bytes
        );
    }
    output.push_str("channels:\n");
    for channel in &info.channels {
        let layout = channel
            .metadata
            .get("rbt.layout_hash")
            .map(String::as_str)
            .unwrap_or("-");
        let _ = writeln!(
            output,
            "  {} id={} messages={} bytes={} encoding={} layout={}{}",
            channel.topic,
            channel.id,
            channel.message_count,
            channel.payload_bytes,
            channel.message_encoding,
            layout,
            channel
                .unsupported_reason
                .as_ref()
                .map_or_else(String::new, |reason| format!(" unsupported={reason}")),
        );
    }
    if let Some((_, summary)) = info
        .metadata
        .iter()
        .rev()
        .find(|(name, _)| name == "rbt.recording.summary")
    {
        let _ = writeln!(
            output,
            "recording: complete={} drops={} gaps={} writer_errors={}",
            summary.get("complete").map(String::as_str).unwrap_or("-"),
            summary
                .get("recorder_drops")
                .map(String::as_str)
                .unwrap_or("-"),
            summary
                .get("transport_sequence_gaps")
                .map(String::as_str)
                .unwrap_or("-"),
            summary
                .get("writer_errors")
                .map(String::as_str)
                .unwrap_or("-"),
        );
    }
    output
}

fn find_capability<'a>(
    view: &'a InspectionView,
    query: &str,
) -> Result<&'a CapabilityInspection, CliError> {
    let by_service = view
        .capability_topology
        .iter()
        .find(|capability| capability.physical_endpoint == query);
    let by_instance = Uuid::parse_str(query).ok().and_then(|uuid| {
        view.capability_topology
            .iter()
            .find(|capability| capability.capability_instance.as_uuid() == &uuid)
    });
    if let Some(capability) = by_service.or(by_instance) {
        return Ok(capability);
    }
    let logical = view
        .capability_topology
        .iter()
        .filter(|capability| capability.capability_type == query)
        .collect::<Vec<_>>();
    match logical.as_slice() {
        [capability] => Ok(*capability),
        [] => Err(CliError::NotFound(format!(
            "capability {query:?} is not active; run `rbt capability list` to see logical selectors"
        ))),
        _ => Err(CliError::NotFound(format!(
            "capability selector {query:?} is ambiguous ({} active instances); use the instance ID from `rbt capability list --verbose`",
            logical.len()
        ))),
    }
}

fn connection_name(component: &ComponentInspection) -> &'static str {
    match component.control_connection {
        rbt_compose::ControlConnectionStatus::Connected => "connected",
        rbt_compose::ControlConnectionStatus::Disconnected => "disconnected",
        rbt_compose::ControlConnectionStatus::Conflict => "conflict",
    }
}

fn lifecycle_freshness(component: &ComponentInspection) -> &'static str {
    if component.snapshot_received
        && matches!(
            component.control_connection,
            rbt_compose::ControlConnectionStatus::Connected
        )
    {
        "age_unavailable"
    } else {
        "stale_age_unavailable"
    }
}

fn lifecycle_age_label(component: &ComponentInspection) -> &'static str {
    lifecycle_age_label_for_freshness(lifecycle_freshness(component))
}

fn lifecycle_age_label_for_freshness(freshness: &str) -> &'static str {
    if freshness == "age_unavailable" {
        "unavailable"
    } else {
        "stale/age unavailable"
    }
}

fn state_name(state: &ComponentState) -> &'static str {
    match state {
        ComponentState::Declared => "DECLARED",
        ComponentState::Preparing => "PREPARING",
        ComponentState::Blocked(_) => "BLOCKED",
        ComponentState::Activating => "ACTIVATING",
        ComponentState::Active => "ACTIVE",
        ComponentState::Quiescing => "QUIESCING",
        ComponentState::Failed(_) => "FAILED",
    }
}

fn phase_name(view: &InspectionView) -> &'static str {
    match view.phase {
        rbt_compose::ComposerPhase::Recovering => "recovering",
        rbt_compose::ComposerPhase::Running => "running",
    }
}

fn display_uuid<T: Copy + Into<Uuid>>(value: T, full: bool) -> String {
    let uuid: Uuid = value.into();
    let text = uuid.to_string();
    if full { text } else { text[..8].to_owned() }
}

fn profile_name(profile: rbt_core::StreamProfile) -> &'static str {
    match profile {
        rbt_core::StreamProfile::Sensor => "sensor",
        rbt_core::StreamProfile::Control => "control",
    }
}

fn transport_label(kind: Option<CapabilityTransportKind>) -> &'static str {
    match kind {
        Some(CapabilityTransportKind::Rpc) => "RPC",
        Some(CapabilityTransportKind::Stream) => "Stream",
        None => "Unknown",
    }
}

fn dot_escape(value: &str) -> String {
    value.replace('\\', "\\\\").replace('"', "\\\"")
}

#[cfg(test)]
mod tests {
    use super::*;
    use rbt_compose::{
        ActivationSnapshot, ActiveContext, CapabilityInspection, CapabilitySnapshot,
        ComponentInspection, ComposerPhase, ControlConnectionStatus, PendingInspection,
    };
    use rbt_core::{
        ActivationId, CapabilityDescriptor, CapabilityInstanceId, ComponentId, ComponentState,
        ComposerEpoch, ProcessInstanceId, RequirementSpec, Revision,
    };
    use rbt_types::{Detection, Frame, StreamDescriptor, StreamProfile};

    #[tokio::test]
    async fn fetch_view_timeout_is_bounded_and_mentions_address() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
            .await
            .expect("listener should bind");
        let address = listener
            .local_addr()
            .expect("listener should have an address")
            .to_string();
        let server = tokio::spawn(async move {
            let (_stream, _) = listener.accept().await.expect("client should connect");
            tokio::time::sleep(Duration::from_secs(1)).await;
        });

        let started = std::time::Instant::now();
        let error = fetch_view_with_timeout(address.clone(), Duration::from_millis(100))
            .await
            .expect_err("an unresponsive inspection server should time out");
        let elapsed = started.elapsed();
        assert!(
            elapsed < Duration::from_millis(500),
            "timeout should be bounded, elapsed={elapsed:?}"
        );
        match error {
            CliError::Inspect(error) => {
                assert_eq!(error.kind(), io::ErrorKind::TimedOut);
                assert!(
                    error
                        .to_string()
                        .contains(&format!("Composer unavailable at {address}:"))
                );
                assert!(error.to_string().contains(
                    "start Composer or verify --composer/RBT_COMPOSER_ADDR/project config"
                ));
            }
            other => panic!("expected a timed-out inspection error, got {other:?}"),
        }
        server.abort();
    }

    fn view() -> InspectionView {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::from_uuid(Uuid::from_u128(1));
        let snapshot = rbt_compose::ComponentSnapshot::new(
            component.clone(),
            process,
            ComponentState::Blocked("camera unavailable".into()),
            None,
            [RequirementSpec::new("camera", "Camera")],
            [],
        );
        InspectionView {
            epoch: ComposerEpoch::from_uuid(Uuid::from_u128(2)),
            revision: Revision::new(ComposerEpoch::from_uuid(Uuid::from_u128(2)), 4),
            phase: ComposerPhase::Running,
            components: vec![ComponentInspection {
                component_id: component,
                snapshot,
                snapshot_received: true,
                control_connection: ControlConnectionStatus::Connected,
                pending: Some(PendingInspection {
                    operation: "activate".to_owned(),
                    revision: Revision::new(ComposerEpoch::from_uuid(Uuid::from_u128(2)), 3),
                    reason: "waiting for provider".to_owned(),
                }),
                last_active: Some(ActiveContext {
                    process_instance: process,
                    activation_id: ActivationId::first(),
                    bindings: Vec::new(),
                    capabilities: Vec::new(),
                }),
            }],
            capability_topology: vec![CapabilityInspection {
                capability_type: "Camera".to_owned(),
                capability_instance: CapabilityInstanceId::from_uuid(Uuid::from_u128(3)),
                provider_component: ComponentId::from("camera"),
                provider_process: process,
                activation: ActivationId::first(),
                physical_endpoint: "rbt/cap/00000000-0000-0000-0000-000000000003/stream".to_owned(),
                transport: CapabilityTransportInspection::Stream {
                    message_type: rbt_core::MessageTypeDescriptor::from_fixed::<Frame>(),
                    profile: StreamProfile::Sensor,
                    readiness: rbt_core::ReadinessProtocol::SidecarV1,
                    queue_depth: 1,
                    history_size: None,
                    safe_overflow: true,
                    resources: rbt_core::StreamResourceConfig::default(),
                    pressure: None,
                    pressure_freshness: None,
                    subscriber_observations: Vec::new(),
                    assessments: Vec::new(),
                },
            }],
            process_conflicts: Vec::new(),
        }
    }

    fn snapshot_capability(
        instance: CapabilityInstanceId,
        provider: ComponentId,
        process: ProcessInstanceId,
    ) -> CapabilitySnapshot {
        CapabilitySnapshot::new(
            "Camera",
            instance,
            CapabilityDescriptor::new(
                instance,
                provider,
                process,
                ActivationId::first(),
                StreamDescriptor::new(
                    format!("rbt/test/{instance}"),
                    Frame::descriptor(),
                    StreamProfile::Sensor,
                ),
            ),
        )
    }

    #[test]
    fn why_distinguishes_missing_requirements() {
        let mut blocked = view();
        blocked.capability_topology.clear();
        let output = format_why(&blocked, "detector").unwrap();
        assert!(output.contains("missing"));
        assert!(output.contains("camera"));
    }

    #[test]
    fn why_uses_compact_ids_by_default_and_full_ids_on_request() {
        let short = format_why_with_options(&view(), "detector", false).unwrap();
        let full = format_why_with_options(&view(), "detector", true).unwrap();

        assert!(short.contains("capability=00000000 transport=Stream state=ACTIVE"));
        assert!(short.contains("process: 00000000"));
        assert!(!short.contains("00000000-0000-0000-0000-000000000001"));
        assert!(full.contains("capability=00000000-0000-0000-0000-000000000003"));
        assert!(full.contains("process: 00000000-0000-0000-0000-000000000001"));
        assert!(full.contains("activation: 1"));
    }

    #[test]
    fn why_reports_failed_candidate_reason_for_active_topology() {
        let mut failed = view();
        failed.components.push(ComponentInspection {
            component_id: ComponentId::from("camera"),
            snapshot: rbt_compose::ComponentSnapshot::new(
                ComponentId::from("camera"),
                ProcessInstanceId::from_uuid(Uuid::from_u128(4)),
                ComponentState::Failed("critical task exited".to_owned()),
                None,
                std::iter::empty::<RequirementSpec>(),
                [],
            ),
            snapshot_received: true,
            control_connection: ControlConnectionStatus::Connected,
            pending: None,
            last_active: None,
        });

        let why: serde_json::Value =
            serde_json::from_str(&format_why_json(&failed, "detector").unwrap()).unwrap();
        let candidate = &why["requirements"][0]["candidates"][0];
        assert_eq!(candidate["state"], "FAILED");
        assert_eq!(candidate["failure_reason"], "critical task exited");
    }

    #[test]
    fn why_reports_failed_candidate_reason_for_retained_and_last_active_capabilities() {
        let retained_provider = ComponentId::from("retained-camera");
        let retained_process = ProcessInstanceId::from_uuid(Uuid::from_u128(5));
        let retained_instance = CapabilityInstanceId::from_uuid(Uuid::from_u128(5));
        let mut retained = view();
        retained.capability_topology.clear();
        retained.components.push(ComponentInspection {
            component_id: retained_provider.clone(),
            snapshot: rbt_compose::ComponentSnapshot::new(
                retained_provider.clone(),
                retained_process,
                ComponentState::Failed("retained provider failed".to_owned()),
                None,
                std::iter::empty::<RequirementSpec>(),
                [snapshot_capability(
                    retained_instance,
                    retained_provider,
                    retained_process,
                )],
            ),
            snapshot_received: true,
            control_connection: ControlConnectionStatus::Connected,
            pending: None,
            last_active: None,
        });
        let retained_why: serde_json::Value =
            serde_json::from_str(&format_why_json(&retained, "detector").unwrap()).unwrap();
        let retained_candidate = retained_why["requirements"][0]["candidates"]
            .as_array()
            .unwrap()
            .iter()
            .find(|candidate| candidate["capability_instance"] == retained_instance.to_string())
            .cloned()
            .expect("retained candidate should be reported");
        assert_eq!(
            retained_candidate["failure_reason"],
            "retained provider failed"
        );

        let last_provider = ComponentId::from("last-camera");
        let last_process = ProcessInstanceId::from_uuid(Uuid::from_u128(6));
        let last_instance = CapabilityInstanceId::from_uuid(Uuid::from_u128(6));
        let mut last_active = view();
        let mut last_capability = last_active.capability_topology[0].clone();
        last_capability.provider_component = last_provider.clone();
        last_capability.provider_process = last_process;
        last_capability.capability_instance = last_instance;
        last_active.capability_topology.clear();
        last_active.components.push(ComponentInspection {
            component_id: last_provider.clone(),
            snapshot: rbt_compose::ComponentSnapshot::new(
                last_provider.clone(),
                last_process,
                ComponentState::Failed("last provider failed".to_owned()),
                None,
                std::iter::empty::<RequirementSpec>(),
                [],
            ),
            snapshot_received: true,
            control_connection: ControlConnectionStatus::Connected,
            pending: None,
            last_active: Some(ActiveContext {
                process_instance: last_process,
                activation_id: ActivationId::first(),
                bindings: Vec::new(),
                capabilities: vec![last_capability],
            }),
        });
        let last_why: serde_json::Value =
            serde_json::from_str(&format_why_json(&last_active, "detector").unwrap()).unwrap();
        let last_candidate = last_why["requirements"][0]["candidates"]
            .as_array()
            .unwrap()
            .iter()
            .find(|candidate| candidate["capability_instance"] == last_instance.to_string())
            .cloned()
            .expect("last-active candidate should be reported");
        assert_eq!(last_candidate["failure_reason"], "last provider failed");
    }

    #[test]
    fn why_failed_state_reason_recognizes_exit_and_freshness_labels_are_explicit() {
        let mut failed = view();
        failed.components[0].snapshot.state =
            ComponentState::Failed("critical task inference exited".to_owned());
        let why: serde_json::Value =
            serde_json::from_str(&format_why_json(&failed, "detector").unwrap()).unwrap();
        assert!(
            why["reason_codes"]
                .as_array()
                .unwrap()
                .contains(&serde_json::Value::String(
                    "critical_task_failed".to_owned()
                ))
        );

        let human = format_why(&failed, "detector").unwrap();
        assert!(human.contains("lifecycle age: unavailable"));
        assert!(!human.contains("lifecycle age: unknown"));

        failed.components[0].snapshot_received = false;
        let stale = format_why(&failed, "detector").unwrap();
        assert!(stale.contains("lifecycle age: stale/age unavailable"));
    }

    #[test]
    fn ps_has_required_columns_and_full_uuid_switch() {
        let short = format_ps(&view(), false);
        let full = format_ps(&view(), true);
        assert_eq!(
            short.lines().next(),
            Some("COMPONENT\tSTATE\tPROCESS\tACTIVATION\tAGE\tCONTROL")
        );
        assert!(short.contains("detector\tBLOCKED\t00000000\t-\tunavailable\tconnected"));
        assert!(full.contains(
            "detector\tBLOCKED\t00000000-0000-0000-0000-000000000001\t-\tunavailable\tconnected"
        ));
        assert!(full.contains("epoch=00000000-0000-0000-0000-000000000002"));

        let ps: serde_json::Value =
            serde_json::from_str(&format_ps_json(&view()).unwrap()).unwrap();
        assert_eq!(
            ps["components"][0]["lifecycle_age_ms"],
            serde_json::Value::Null
        );
        assert_eq!(
            ps["components"][0]["lifecycle_freshness"],
            "age_unavailable"
        );

        let mut stale_snapshot = view();
        stale_snapshot.components[0].snapshot_received = false;
        let stale_snapshot_ps: serde_json::Value =
            serde_json::from_str(&format_ps_json(&stale_snapshot).unwrap()).unwrap();
        assert_eq!(
            stale_snapshot_ps["components"][0]["lifecycle_freshness"],
            "stale_age_unavailable"
        );

        let mut stale_connection = view();
        stale_connection.components[0].control_connection = ControlConnectionStatus::Disconnected;
        let stale_connection_ps: serde_json::Value =
            serde_json::from_str(&format_ps_json(&stale_connection).unwrap()).unwrap();
        assert_eq!(
            stale_connection_ps["components"][0]["lifecycle_freshness"],
            "stale_age_unavailable"
        );
    }

    #[test]
    fn ps_json_preserves_component_subscriber_pressure_records() {
        let pressure = rbt_core::StreamPressure {
            service_name: "rbt/cap/disappeared/stream".to_owned(),
            profile: StreamProfile::Sensor,
            semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            resources: rbt_core::StreamResourceConfig::default(),
            publisher: None,
            subscriber: Some(rbt_core::SubscriberPressure {
                received_samples: 7,
                ..Default::default()
            }),
            last_pressure_event: None,
            snapshot_timestamp_unix_ms: 123,
        };
        let mut inspection = view();
        inspection.components[0]
            .snapshot
            .stream_pressures
            .push(pressure.clone());

        let ps: serde_json::Value =
            serde_json::from_str(&format_ps_json(&inspection).unwrap()).unwrap();
        assert_eq!(
            ps["components"][0]["stream_pressures"],
            serde_json::json!([serde_json::to_value(pressure).unwrap()])
        );
    }

    #[test]
    fn ps_keeps_numeric_activation_ids_compact_in_both_modes() {
        let mut inspection = view();
        inspection.components[0].snapshot.activation =
            Some(ActivationSnapshot::new(ActivationId::new(42), []));
        let short = format_ps(&inspection, false);
        let full = format_ps(&inspection, true);
        assert!(short.contains("\t42\tunavailable\tconnected"));
        assert!(full.contains("\t42\tunavailable\tconnected"));
    }

    #[test]
    fn capability_list_has_semantic_and_physical_columns() {
        let output = format_capability_list(&view());
        assert_eq!(
            output.lines().next(),
            Some(
                "TYPE\tNAME\tPROVIDER\tCAPABILITY_INSTANCE\tTRANSPORT\tMESSAGE\tREQUEST\tRESPONSE\tPROFILE"
            )
        );
        let row = output.lines().nth(1).unwrap();
        assert!(row.contains("Camera\t-\tcamera\t00000000"));
        assert!(!row.contains("rbt/cap/"));
        assert!(row.contains(&Frame::descriptor().qualified_name()));
        assert!(row.ends_with("\tsensor"));

        let verbose = format_capability_list_verbose(&view());
        assert!(verbose.lines().next().unwrap().contains("ENDPOINT"));
        assert!(verbose.contains("rbt/cap/00000000-0000-0000-0000-000000000003/stream"));
    }

    fn rpc_view(with_metrics: bool) -> InspectionView {
        let mut inspection = view();
        let capability = &mut inspection.capability_topology[0];
        capability.physical_endpoint =
            "rbt/cap/00000000-0000-0000-0000-000000000003/rpc".to_owned();
        let server_pressure = with_metrics.then(|| rbt_core::RpcPressure {
            service_name: capability.physical_endpoint.clone(),
            capability_instance: Some(capability.capability_instance),
            resources: rbt_core::RpcResourceConfig::default(),
            client_activation: None,
            client: None,
            server: Some(rbt_core::RpcServerMetrics::default()),
            snapshot_timestamp_unix_ms: 10_000,
        });
        let server_pressure_freshness = with_metrics.then_some(PressureFreshness {
            snapshot_timestamp_unix_ms: 10_000,
            age_ms: 25,
            fresh: true,
        });
        let client_observations = if with_metrics {
            vec![RpcClientPressureObservation {
                component_id: ComponentId::from("detector"),
                process_instance_id: ProcessInstanceId::from_uuid(Uuid::from_u128(5)),
                activation_id: Some(ActivationId::first()),
                pressure: rbt_core::RpcPressure {
                    service_name: capability.physical_endpoint.clone(),
                    capability_instance: Some(capability.capability_instance),
                    resources: rbt_core::RpcResourceConfig::default(),
                    client_activation: Some(ActivationId::first()),
                    client: Some(rbt_core::RpcClientMetrics {
                        calls_attempted: 2,
                        calls_completed: 1,
                        calls_timed_out: 1,
                        ..Default::default()
                    }),
                    server: None,
                    snapshot_timestamp_unix_ms: 10_000,
                },
                age_ms: 25,
                fresh: true,
            }]
        } else {
            Vec::new()
        };
        capability.transport = CapabilityTransportInspection::Rpc {
            request_type: Frame::descriptor(),
            response_type: rbt_core::MessageTypeDescriptor::from_fixed::<Detection>(),
            readiness: rbt_core::RpcReadinessProtocol::RpcSidecarV1,
            resources: rbt_core::RpcResourceConfig::default(),
            server_pressure,
            server_pressure_freshness,
            client_observations,
        };
        inspection
    }

    #[test]
    fn rpc_info_reports_typed_metrics_without_fabricating_unavailable_zeroes() {
        let endpoint = "rbt/cap/00000000-0000-0000-0000-000000000003/rpc";
        let unavailable: serde_json::Value =
            serde_json::from_str(&format_rpc_info_json(&rpc_view(false), endpoint).unwrap())
                .unwrap();
        assert_eq!(unavailable["transport_kind"], "rpc");
        assert_eq!(unavailable["metrics_status"], "unavailable");
        assert!(unavailable["server_pressure"].is_null());
        assert_eq!(unavailable["client_observations"], serde_json::json!([]));

        let available_view = rpc_view(true);
        let available: serde_json::Value =
            serde_json::from_str(&format_rpc_info_json(&available_view, endpoint).unwrap())
                .unwrap();
        assert_eq!(available["metrics_status"], "available");
        assert_eq!(
            available["server_pressure"]["server"]["requests_received"],
            0
        );
        assert_eq!(
            available["client_observations"][0]["pressure"]["client"]["calls_timed_out"],
            1
        );
        let human = format_rpc_info(&available_view, endpoint, true).unwrap();
        assert!(human.contains("transport: RPC"));
        assert!(human.contains("server (fresh): requests received=0"));
        assert!(human.contains("attempted=2 completed=1"));
        assert!(
            format_stream_info(&available_view, endpoint)
                .unwrap_err()
                .to_string()
                .contains("rbt rpc info")
        );
    }

    #[test]
    fn graph_dot_is_stable_and_escaped() {
        let inspection = view();
        let output = format_graph_dot(&inspection);
        assert_eq!(output, format_graph_dot(&inspection));
        assert!(output.contains("component:detector"));
        assert!(output.contains("component:camera"));
        assert!(output.contains("requirement:detector:camera"));
        assert!(output.contains("[unresolved]"));
        assert!(output.contains("provides:Camera"));
    }

    #[test]
    fn graph_json_distinguishes_components_requirements_and_capabilities() {
        let graph: serde_json::Value =
            serde_json::from_str(&format_graph_json(&view()).unwrap()).unwrap();
        let nodes = graph["nodes"].as_array().unwrap();
        assert!(nodes.iter().any(|node| node["kind"] == "component"));
        assert!(nodes.iter().any(|node| {
            node["kind"] == "requirement"
                && node["resolved"] == false
                && node["status"] == "unresolved"
        }));
        assert!(nodes.iter().any(|node| node["kind"] == "capability"));
        assert_eq!(
            graph["freshness"]["lifecycle_age_ms"],
            serde_json::Value::Null
        );
        assert_eq!(graph["freshness"]["lifecycle_freshness"], "age_unavailable");
        assert_eq!(
            graph["freshness"]["components"][0]["lifecycle_age_ms"],
            serde_json::Value::Null
        );
        assert_eq!(
            graph["freshness"]["components"][0]["lifecycle_freshness"],
            "age_unavailable"
        );
    }

    #[test]
    fn why_json_uses_typed_reasons_without_inventing_provider_loss() {
        let why: serde_json::Value =
            serde_json::from_str(&format_why_json(&view(), "detector").unwrap()).unwrap();
        assert!(
            why["reason_codes"]
                .as_array()
                .unwrap()
                .contains(&serde_json::Value::String(
                    "requirement_unresolved".to_owned()
                ))
        );
        assert!(
            !why["reason_codes"]
                .as_array()
                .unwrap()
                .contains(&serde_json::Value::String(
                    "provider_disappeared".to_owned()
                ))
        );
        assert_eq!(
            why["requirements"][0]["candidates"][0]["provider"],
            "camera"
        );
    }

    #[test]
    fn stream_info_reports_profile_derived_queue_policy() {
        let output = format_stream_info(
            &view(),
            "rbt/cap/00000000-0000-0000-0000-000000000003/stream",
        )
        .unwrap();
        assert!(output.contains("queue depth: 1"));
        assert!(output.contains("safe overflow: true"));
        assert!(output.contains("layout hash:"));
        assert!(output.contains("readiness: sidecar-v1"));
        assert!(!output.contains("physical endpoint:"));
        assert!(
            format_stream_info_verbose(
                &view(),
                "rbt/cap/00000000-0000-0000-0000-000000000003/stream",
            )
            .unwrap()
            .contains("physical endpoint:")
        );
    }

    #[test]
    fn stream_pressure_never_fabricates_unreported_counters() {
        let output = format_stream_pressure(
            &view(),
            "rbt/cap/00000000-0000-0000-0000-000000000003/stream",
        )
        .unwrap();
        assert!(output.contains("sent: unknown"));
        assert!(output.contains("connected: unknown"));
        assert!(output.contains("last_event: unknown"));
        assert!(output.contains("snapshot_age_ms: unknown"));
        assert!(output.contains("snapshot_status: unavailable"));
        assert!(!output.contains("sent: 0"));
    }

    #[test]
    fn stream_json_keeps_unavailable_pressure_distinct_from_zero() {
        let endpoint = "rbt/cap/00000000-0000-0000-0000-000000000003/stream";
        let info: serde_json::Value =
            serde_json::from_str(&format_stream_info_json(&view(), endpoint).unwrap()).unwrap();
        assert_eq!(info["semantic"]["queue_depth"], 1);
        assert!(info["pressure"].is_null());
        assert!(info["pressure_freshness"].is_null());
        assert_eq!(info["subscriber_observations"].as_array().unwrap().len(), 0);

        let pressure: serde_json::Value =
            serde_json::from_str(&format_stream_pressure_json(&view(), endpoint).unwrap()).unwrap();
        assert_eq!(pressure["semantic"]["queue_depth"], 1);
        assert!(pressure["semantic"]["history_size"].is_null());
        assert_eq!(pressure["semantic"]["safe_overflow"], true);
        assert_eq!(pressure["resources"]["max_loans"], 2);
        assert_eq!(pressure["resources"]["max_borrowed"], 1);
        assert!(pressure["snapshot"].is_null());
        assert!(pressure["publisher"].is_null());
        assert_eq!(
            pressure["subscriber_observations"]
                .as_array()
                .unwrap()
                .len(),
            0
        );
        assert!(pressure["snapshot_freshness"].is_null());
    }

    #[test]
    fn stream_pressure_reports_complete_subscriber_observation() {
        let mut inspection = view();
        let endpoint = inspection.capability_topology[0].physical_endpoint.clone();
        let CapabilityTransportInspection::Stream {
            resources,
            subscriber_observations,
            ..
        } = &mut inspection.capability_topology[0].transport
        else {
            panic!("test fixture must be a Stream capability");
        };
        *resources = rbt_core::StreamResourceConfig::new(7, 3);
        *subscriber_observations = vec![SubscriberPressureObservation {
            component_id: ComponentId::from("viewer"),
            process_instance_id: ProcessInstanceId::from_uuid(Uuid::from_u128(4)),
            activation_id: Some(ActivationId::first()),
            pressure: rbt_core::StreamPressure {
                service_name: endpoint,
                profile: StreamProfile::Sensor,
                semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
                resources: rbt_core::StreamResourceConfig::new(11, 5),
                publisher: None,
                subscriber: Some(rbt_core::SubscriberPressure {
                    receive_attempts: 12,
                    received_samples: 10,
                    borrowed_samples: 2,
                    borrowed_high_water: 3,
                    borrow_failures: 4,
                    borrow_duration_count: 5,
                    borrow_duration_total_ns: 600,
                    borrow_duration_max_ns: 200,
                    oldest_borrow_age_ns: Some(700),
                    estimated_dropped_samples: None,
                }),
                last_pressure_event: None,
                snapshot_timestamp_unix_ms: 10_000,
            },
            age_ms: 42,
            fresh: true,
            assessments: Vec::new(),
        }];
        let endpoint = "rbt/cap/00000000-0000-0000-0000-000000000003/stream";

        let output = format_stream_pressure(&inspection, endpoint).unwrap();
        for metric in [
            "receive_attempts=12",
            "received_samples=10",
            "borrowed_samples=2",
            "borrowed_high_water=3",
            "borrow_failures=4",
            "borrow_duration_count=5",
            "borrow_duration_total_ns=600",
            "borrow_duration_max_ns=200",
            "oldest_borrow_age_ns=700",
            "estimated_dropped_samples=unknown",
        ] {
            assert!(output.contains(metric), "missing {metric} in {output}");
        }
        assert!(output.contains("resources:\n  max loans: 7\n  max borrowed: 3"));

        let pressure: serde_json::Value =
            serde_json::from_str(&format_stream_pressure_json(&inspection, endpoint).unwrap())
                .unwrap();
        let subscriber = &pressure["subscriber_observations"][0]["pressure"];
        assert_eq!(subscriber["resources"]["max_loans"], 11);
        assert_eq!(subscriber["resources"]["max_borrowed"], 5);
        assert_eq!(subscriber["subscriber"]["receive_attempts"], 12);
        assert_eq!(subscriber["subscriber"]["received_samples"], 10);
        assert_eq!(subscriber["subscriber"]["borrowed_samples"], 2);
        assert_eq!(subscriber["subscriber"]["borrowed_high_water"], 3);
        assert_eq!(subscriber["subscriber"]["borrow_failures"], 4);
        assert_eq!(subscriber["subscriber"]["borrow_duration_count"], 5);
        assert_eq!(subscriber["subscriber"]["borrow_duration_total_ns"], 600);
        assert_eq!(subscriber["subscriber"]["borrow_duration_max_ns"], 200);
        assert_eq!(subscriber["subscriber"]["oldest_borrow_age_ns"], 700);
        assert!(subscriber["subscriber"]["estimated_dropped_samples"].is_null());
    }

    #[test]
    fn stream_pressure_non_publisher_snapshot_is_not_current_zero() {
        let mut inspection = view();
        let endpoint_name = inspection.capability_topology[0].physical_endpoint.clone();
        let CapabilityTransportInspection::Stream { pressure, .. } =
            &mut inspection.capability_topology[0].transport
        else {
            panic!("test fixture must be a Stream capability");
        };
        *pressure = Some(rbt_core::StreamPressure {
            service_name: endpoint_name,
            profile: StreamProfile::Sensor,
            semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            resources: rbt_core::StreamResourceConfig::default(),
            publisher: None,
            subscriber: Some(rbt_core::SubscriberPressure::default()),
            last_pressure_event: None,
            snapshot_timestamp_unix_ms: 10_000,
        });
        let endpoint = "rbt/cap/00000000-0000-0000-0000-000000000003/stream";

        let output = format_stream_pressure(&inspection, endpoint).unwrap();
        assert!(output.contains("source: unavailable"));
        assert!(output.contains("sent: unknown"));
        assert!(output.contains("snapshot_status: unavailable"));
        assert!(!output.contains("sent: 0"));

        let pressure: serde_json::Value =
            serde_json::from_str(&format_stream_pressure_json(&inspection, endpoint).unwrap())
                .unwrap();
        assert!(pressure["snapshot"].is_object());
        assert!(pressure["publisher"].is_null());
        assert!(pressure["snapshot_freshness"].is_null());
    }

    fn stale_view() -> InspectionView {
        let mut inspection = view();
        let snapshot_timestamp_unix_ms = 10_000;
        let endpoint_name = inspection.capability_topology[0].physical_endpoint.clone();
        let CapabilityTransportInspection::Stream {
            pressure,
            pressure_freshness,
            ..
        } = &mut inspection.capability_topology[0].transport
        else {
            panic!("test fixture must be a Stream capability");
        };
        *pressure = Some(rbt_core::StreamPressure {
            service_name: endpoint_name,
            profile: StreamProfile::Sensor,
            semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            resources: rbt_core::StreamResourceConfig::default(),
            publisher: Some(rbt_core::PublisherPressure::default()),
            subscriber: None,
            last_pressure_event: None,
            snapshot_timestamp_unix_ms,
        });
        *pressure_freshness = Some(PressureFreshness {
            snapshot_timestamp_unix_ms,
            age_ms: rbt_compose::PRESSURE_FRESHNESS_WINDOW_MS + 17,
            fresh: false,
        });
        inspection
    }

    #[test]
    fn stream_pressure_human_surface_reports_stale_age() {
        let output = format_stream_pressure(
            &stale_view(),
            "rbt/cap/00000000-0000-0000-0000-000000000003/stream",
        )
        .unwrap();

        assert!(output.contains(&format!(
            "snapshot_age_ms: {}",
            rbt_compose::PRESSURE_FRESHNESS_WINDOW_MS + 17
        )));
        assert!(output.contains("snapshot_status: stale"));
        assert!(!output.contains("snapshot_age_ms: unknown"));
    }

    #[test]
    fn stream_pressure_json_surface_reports_stale_age() {
        let endpoint = "rbt/cap/00000000-0000-0000-0000-000000000003/stream";
        let info: serde_json::Value =
            serde_json::from_str(&format_stream_info_json(&stale_view(), endpoint).unwrap())
                .unwrap();
        assert_eq!(
            info["pressure_freshness"]["age_ms"],
            rbt_compose::PRESSURE_FRESHNESS_WINDOW_MS + 17
        );
        assert_eq!(info["pressure_freshness"]["fresh"], false);

        let pressure: serde_json::Value =
            serde_json::from_str(&format_stream_pressure_json(&stale_view(), endpoint).unwrap())
                .unwrap();

        assert_eq!(
            pressure["snapshot_freshness"]["snapshot_timestamp_unix_ms"],
            10_000
        );
        assert_eq!(
            pressure["snapshot_freshness"]["age_ms"],
            rbt_compose::PRESSURE_FRESHNESS_WINDOW_MS + 17
        );
        assert_eq!(pressure["snapshot_freshness"]["fresh"], false);
    }
}

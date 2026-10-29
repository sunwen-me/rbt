//! Partial TF diagnostics from Composer metadata or an explicitly selected
//! consumer-local buffer observation.

use std::{
    collections::{BTreeMap, BTreeSet},
    fmt::Write as _,
    fs::File,
    io::Read,
    path::Path,
};

use rbt_compose::{
    CapabilityInspection, CapabilityTransportInspection, ComponentInspection, InspectionView,
};
use rbt_time::{HostClock, TimeError, Timestamp};
use rbt_types::{MessageShape, MessageTypeDescriptor};
use serde::{Deserialize, Serialize};

const MAX_SNAPSHOT_BYTES: u64 = 1_048_576;
const MAX_PROVIDERS: usize = 256;
const MAX_EDGES: usize = 4096;
const MAX_FRAMES: usize = 8192;
const MAX_SAMPLES: usize = 1_000_000;
const MAX_COUNTER: u64 = 1_000_000_000_000_000;
const MAX_AGE_NS: u64 = 3_153_600_000_000_000_000;
const FRESHNESS_LIMIT_NS: u64 = 2_500_000_000;

/// Stable TF endpoint metadata shown from a live Composer inspection.
#[derive(Clone, Debug, Serialize)]
pub struct TfProviderStatus {
    pub capability_type: String,
    pub capability_instance: String,
    pub provider_component: String,
    pub provider_process: String,
    pub activation: u64,
    pub physical_endpoint: String,
    pub transport_kind: &'static str,
    pub descriptors: Vec<TfEndpointDescriptor>,
    pub consumers: Vec<TfBindingConsumer>,
}

#[derive(Clone, Debug, Serialize)]
pub struct TfEndpointDescriptor {
    pub role: &'static str,
    pub message_type: MessageTypeDescriptor,
}

#[derive(Clone, Debug, Serialize)]
pub struct TfBindingConsumer {
    pub component: String,
    pub process: String,
    pub activation: u64,
    pub connection: &'static str,
    pub requirement: String,
    pub capability_type: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct TfGraphStatus {
    /// Composer does not expose TF payload edges. Explicit local snapshots do.
    pub availability: &'static str,
    pub reason_code: Option<String>,
    /// `None` means unavailable; `Some(vec![])` is a measured empty graph.
    pub edges: Option<Vec<TfGraphEdge>>,
}

#[derive(Clone, Debug, Serialize)]
pub struct TfGraphEdge {
    pub parent: String,
    pub child: String,
    pub source: TfAuthority,
    pub state: &'static str,
    pub revision: Option<u64>,
    pub transform_epoch: Option<u64>,
    pub stored_samples: usize,
    pub conflicting_claims: usize,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct TfAuthority {
    pub slot: String,
    pub physical: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct TfLookupMetrics {
    pub availability: &'static str,
    pub attempts: Option<u64>,
    pub successes: Option<u64>,
    pub failures: Option<u64>,
    pub missing_transform_count: Option<u64>,
    pub missing_frame_count: Option<u64>,
    pub extrapolation_count: Option<u64>,
    pub clock_error_count: Option<u64>,
    pub clock_mapping_failure_count: Option<u64>,
    pub conflict_count: Option<u64>,
    pub authority_conflict_count: Option<u64>,
}

#[derive(Clone, Debug, Serialize)]
pub struct TfObservationFreshness {
    pub status: &'static str,
    pub reason_code: String,
    pub age_ns: Option<i64>,
    pub observed_at: Option<Timestamp>,
}

#[derive(Clone, Debug, Serialize)]
pub struct TfSelectedConsumer {
    pub name: Option<String>,
    pub physical_identity: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
pub struct TfLocalObservation {
    pub consumer: TfSelectedConsumer,
    pub freshness: TfObservationFreshness,
    pub buffer: TfBufferObservation,
}

/// Output shape shared by `rbt tf status` and `rbt tf tree`.
#[derive(Clone, Debug, Serialize)]
pub struct TfStatusReport {
    pub availability: &'static str,
    pub source: &'static str,
    /// TF diagnostics only have the partial visibility of their selected source.
    pub graph_scope: &'static str,
    pub composer_availability: &'static str,
    pub reason_code: Option<String>,
    pub details: Option<String>,
    pub providers: Vec<TfProviderStatus>,
    pub graph: TfGraphStatus,
    /// Missing Composer visibility stays null instead of becoming measured zero.
    pub local_lookup_metrics: TfLookupMetrics,
    pub local_observation: Option<TfLocalObservation>,
}

/// An explicitly selected consumer-local TF buffer observation file.
///
/// The file is diagnostic input only. It is not TF payload transport and is
/// never discovered automatically.
#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct TfSnapshotInput {
    #[serde(default)]
    pub observed_at: Option<Timestamp>,
    #[serde(default)]
    pub consumer_name: Option<String>,
    #[serde(default)]
    pub consumer_physical_identity: Option<String>,
    pub buffer: TfBufferObservation,
}

/// Bounded mirror of the public `rbt_tf::TfPressureSnapshot` serialization.
///
/// The TF crate intentionally does not derive `Deserialize` for its live
/// pressure types. This CLI-owned DTO validates untrusted diagnostic files.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct TfBufferObservation {
    pub active_providers: usize,
    pub active_frames: usize,
    pub static_edges: usize,
    pub dynamic_edges: usize,
    pub stored_samples: usize,
    pub evicted_samples: u64,
    pub lookup_attempts: u64,
    pub lookup_successes: u64,
    pub lookup_failures: u64,
    pub missing_transform_count: u64,
    #[serde(default)]
    pub missing_frame_count: Option<u64>,
    pub extrapolation_count: u64,
    pub clock_error_count: u64,
    #[serde(default)]
    pub clock_mapping_failure_count: Option<u64>,
    pub conflict_count: u64,
    #[serde(default)]
    pub authority_conflict_count: Option<u64>,
    pub providers: Vec<TfProviderObservation>,
    pub graph: Vec<TfGraphEdgeObservation>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct TfProviderObservation {
    pub authority: TfAuthority,
    pub static_revision: Option<u64>,
    pub highest_observed_revision: Option<u64>,
    pub static_tracked: bool,
    pub static_stale: bool,
    pub snapshot_age_ns: Option<u64>,
    pub snapshot_fresh: Option<bool>,
    pub dynamic_update_age_ns: Option<u64>,
    pub dynamic_fresh: bool,
    pub active_static_edges: usize,
    pub active_dynamic_edges: usize,
    pub stored_samples: usize,
    pub evicted_samples: u64,
    pub buffer_observed_lookup_attempts: u64,
    pub buffer_observed_lookup_successes: u64,
    pub buffer_observed_lookup_failures: u64,
    pub missing_transform_count: u64,
    #[serde(default)]
    pub missing_frame_count: Option<u64>,
    pub extrapolation_count: u64,
    pub clock_error_count: u64,
    #[serde(default)]
    pub clock_mapping_failure_count: Option<u64>,
    pub conflict_count: u64,
    #[serde(default)]
    pub authority_conflict_count: Option<u64>,
    pub resync_attempts: u64,
    pub resync_failures: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct TfGraphEdgeObservation {
    pub parent: String,
    pub child: String,
    pub authority: TfAuthority,
    pub is_static: bool,
    pub revision: Option<u64>,
    pub transform_epoch: Option<u64>,
    pub stored_samples: usize,
    pub conflicting_claims: usize,
}

#[derive(Debug, thiserror::Error)]
pub enum TfStatusError {
    #[error("cannot read TF diagnostic snapshot: {0}")]
    Io(#[from] std::io::Error),
    #[error("invalid TF diagnostic snapshot: {0}")]
    Json(#[from] serde_json::Error),
    #[error("invalid TF diagnostic snapshot: {0}")]
    Invalid(String),
}

/// Builds a partial report from the immutable Composer view.
pub fn from_inspection(view: &InspectionView) -> TfStatusReport {
    let mut providers = view
        .capability_topology
        .iter()
        .filter_map(tf_provider)
        .collect::<Vec<_>>();
    providers.sort_by(|left, right| {
        left.capability_instance
            .cmp(&right.capability_instance)
            .then_with(|| left.capability_type.cmp(&right.capability_type))
    });
    for provider in &mut providers {
        provider.consumers = consumers_for(view, provider);
    }
    TfStatusReport {
        availability: "available",
        source: "composer_inspection",
        graph_scope: "partial",
        composer_availability: "available",
        reason_code: None,
        details: None,
        providers,
        graph: TfGraphStatus {
            availability: "unavailable",
            reason_code: Some("composer_does_not_expose_tf_payload_graph_or_history".to_owned()),
            edges: None,
        },
        local_lookup_metrics: unavailable_lookup_metrics(),
        local_observation: None,
    }
}

/// Builds a structured unavailable report when Composer inspection failed.
pub fn composer_unavailable(reason: impl Into<String>) -> TfStatusReport {
    TfStatusReport {
        availability: "unavailable",
        source: "composer_inspection",
        graph_scope: "partial",
        composer_availability: "unavailable",
        reason_code: Some("composer_unavailable".to_owned()),
        details: Some(reason.into()),
        providers: Vec::new(),
        graph: TfGraphStatus {
            availability: "unavailable",
            reason_code: Some("composer_view_unavailable".to_owned()),
            edges: None,
        },
        local_lookup_metrics: unavailable_lookup_metrics(),
        local_observation: None,
    }
}

/// Reads and validates one explicitly selected local consumer observation.
pub fn from_snapshot_file(path: &Path) -> Result<TfStatusReport, TfStatusError> {
    let mut bytes = Vec::new();
    File::open(path)?
        .take(MAX_SNAPSHOT_BYTES + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_SNAPSHOT_BYTES {
        return Err(TfStatusError::Invalid("larger than 1 MiB".to_owned()));
    }
    let snapshot: TfSnapshotInput = serde_json::from_slice(&bytes)?;
    validate_snapshot(&snapshot)?;
    let freshness = observation_freshness(snapshot.observed_at);
    let buffer = snapshot.buffer;
    let edges = buffer
        .graph
        .iter()
        .map(|edge| TfGraphEdge {
            parent: edge.parent.clone(),
            child: edge.child.clone(),
            source: edge.authority.clone(),
            state: if edge.is_static { "static" } else { "dynamic" },
            revision: edge.revision,
            transform_epoch: edge.transform_epoch,
            stored_samples: edge.stored_samples,
            conflicting_claims: edge.conflicting_claims,
        })
        .collect();
    let local_lookup_metrics = TfLookupMetrics {
        availability: "available",
        attempts: Some(buffer.lookup_attempts),
        successes: Some(buffer.lookup_successes),
        failures: Some(buffer.lookup_failures),
        missing_transform_count: Some(buffer.missing_transform_count),
        missing_frame_count: buffer.missing_frame_count,
        extrapolation_count: Some(buffer.extrapolation_count),
        clock_error_count: Some(buffer.clock_error_count),
        clock_mapping_failure_count: buffer.clock_mapping_failure_count,
        conflict_count: Some(buffer.conflict_count),
        authority_conflict_count: buffer.authority_conflict_count,
    };
    Ok(TfStatusReport {
        availability: "available",
        source: "selected_local_consumer_snapshot",
        graph_scope: "partial",
        composer_availability: "not_queried",
        reason_code: None,
        details: None,
        providers: Vec::new(),
        graph: TfGraphStatus {
            availability: "available",
            reason_code: None,
            edges: Some(edges),
        },
        local_lookup_metrics,
        local_observation: Some(TfLocalObservation {
            consumer: TfSelectedConsumer {
                name: snapshot.consumer_name,
                physical_identity: snapshot.consumer_physical_identity,
            },
            freshness,
            buffer,
        }),
    })
}

/// Formats a human status summary or an edge-only known TF tree.
pub fn format_human(report: &TfStatusReport, tree: bool) -> String {
    let label = if tree { "TF tree" } else { "TF status" };
    let mut output = String::new();
    let _ = writeln!(
        output,
        "{label}: availability={} source={} graph_scope={}",
        report.availability, report.source, report.graph_scope
    );
    let _ = writeln!(output, "Composer: {}", report.composer_availability);
    if let Some(reason) = &report.reason_code {
        let _ = writeln!(output, "Reason: {reason}");
    }
    if let Some(details) = &report.details {
        let _ = writeln!(output, "Details: {details}");
    }
    if let Some(observation) = &report.local_observation {
        let _ = writeln!(
            output,
            "Selected local consumer: name={} physical_identity={}",
            observation.consumer.name.as_deref().unwrap_or("unknown"),
            observation
                .consumer
                .physical_identity
                .as_deref()
                .unwrap_or("unknown")
        );
        let _ = writeln!(
            output,
            "Observation freshness: {} reason={} age_ns={}",
            observation.freshness.status,
            observation.freshness.reason_code,
            observation
                .freshness
                .age_ns
                .map_or_else(|| "unavailable".to_owned(), |age| age.to_string())
        );
    }
    if tree {
        match &report.graph.edges {
            Some(edges) if edges.is_empty() => {
                output.push_str("Known TF edges: none in this observation\n")
            }
            Some(edges) => {
                output.push_str("Known TF edges (parent -> child):\n");
                for edge in edges {
                    let state = match (edge.state, edge.revision, edge.transform_epoch) {
                        ("static", Some(revision), _) => format!("static revision={revision}"),
                        ("dynamic", _, Some(epoch)) => format!("dynamic epoch={epoch}"),
                        (state, _, _) => state.to_owned(),
                    };
                    let _ = writeln!(
                        output,
                        "  {} -> {} [source={} physical={} state={} samples={} conflicting_claims={}]",
                        edge.parent,
                        edge.child,
                        edge.source.slot,
                        edge.source.physical,
                        state,
                        edge.stored_samples,
                        edge.conflicting_claims
                    );
                }
            }
            None => {
                let _ = writeln!(
                    output,
                    "Known TF edges: unavailable ({})",
                    report.graph.reason_code.as_deref().unwrap_or("unavailable")
                );
            }
        }
    } else {
        let _ = writeln!(output, "TF payload graph: {}", report.graph.availability);
        for provider in &report.providers {
            let _ = writeln!(
                output,
                "Provider {} id={} component={} process={} activation={} endpoint={} transport={}",
                provider.capability_type,
                provider.capability_instance,
                provider.provider_component,
                provider.provider_process,
                provider.activation,
                provider.physical_endpoint,
                provider.transport_kind
            );
            for descriptor in &provider.descriptors {
                let _ = writeln!(output, "  {}: {}", descriptor.role, descriptor.message_type);
            }
            for consumer in &provider.consumers {
                let _ = writeln!(
                    output,
                    "  consumer {} process={} activation={} connection={} requirement={} capability={}",
                    consumer.component,
                    consumer.process,
                    consumer.activation,
                    consumer.connection,
                    consumer.requirement,
                    consumer.capability_type
                );
            }
        }
        if report.providers.is_empty() && report.availability == "available" {
            output.push_str("TF providers: none in this Composer view\n");
        }
    }
    if let Some(metrics) = report.local_observation.as_ref() {
        let _ = writeln!(
            output,
            "Local buffer: providers={} frames={} static_edges={} dynamic_edges={} samples={} evicted_samples={}",
            metrics.buffer.active_providers,
            metrics.buffer.active_frames,
            metrics.buffer.static_edges,
            metrics.buffer.dynamic_edges,
            metrics.buffer.stored_samples,
            metrics.buffer.evicted_samples
        );
    }
    let metrics = &report.local_lookup_metrics;
    let _ = writeln!(
        output,
        "Local lookup metrics: availability={} attempts={} successes={} failures={} missing_transform={} missing_frame={} extrapolation={} clock_error={} clock_mapping_failure={} conflict={} authority_conflict={}",
        metrics.availability,
        optional_metric(metrics.attempts),
        optional_metric(metrics.successes),
        optional_metric(metrics.failures),
        optional_metric(metrics.missing_transform_count),
        optional_metric(metrics.missing_frame_count),
        optional_metric(metrics.extrapolation_count),
        optional_metric(metrics.clock_error_count),
        optional_metric(metrics.clock_mapping_failure_count),
        optional_metric(metrics.conflict_count),
        optional_metric(metrics.authority_conflict_count)
    );
    output
}

fn optional_metric(value: Option<u64>) -> String {
    value.map_or_else(|| "unavailable".to_owned(), |value| value.to_string())
}

fn unavailable_lookup_metrics() -> TfLookupMetrics {
    TfLookupMetrics {
        availability: "unavailable",
        attempts: None,
        successes: None,
        failures: None,
        missing_transform_count: None,
        missing_frame_count: None,
        extrapolation_count: None,
        clock_error_count: None,
        clock_mapping_failure_count: None,
        conflict_count: None,
        authority_conflict_count: None,
    }
}

fn tf_provider(capability: &CapabilityInspection) -> Option<TfProviderStatus> {
    let (transport_kind, descriptors) = match &capability.transport {
        CapabilityTransportInspection::Stream { message_type, .. }
            if is_tf_descriptor(message_type, "DynamicTransform", MessageShape::Fixed) =>
        {
            (
                "stream",
                vec![TfEndpointDescriptor {
                    role: "stream_message",
                    message_type: message_type.clone(),
                }],
            )
        }
        CapabilityTransportInspection::Stream { message_type, .. }
            if is_tf_descriptor(message_type, "StaticUpdate", MessageShape::Slice) =>
        {
            (
                "stream",
                vec![TfEndpointDescriptor {
                    role: "stream_message",
                    message_type: message_type.clone(),
                }],
            )
        }
        CapabilityTransportInspection::Rpc {
            request_type,
            response_type,
            ..
        } if is_tf_descriptor(response_type, "StaticSnapshot", MessageShape::Slice) => (
            "rpc",
            vec![
                TfEndpointDescriptor {
                    role: "rpc_request",
                    message_type: request_type.clone(),
                },
                TfEndpointDescriptor {
                    role: "rpc_response",
                    message_type: response_type.clone(),
                },
            ],
        ),
        _ => return None,
    };
    Some(TfProviderStatus {
        capability_type: capability.capability_type.clone(),
        capability_instance: capability.capability_instance.to_string(),
        provider_component: capability.provider_component.to_string(),
        provider_process: capability.provider_process.to_string(),
        activation: capability.activation.value(),
        physical_endpoint: capability.physical_endpoint.clone(),
        transport_kind,
        descriptors,
        consumers: Vec::new(),
    })
}

fn is_tf_descriptor(descriptor: &MessageTypeDescriptor, name: &str, shape: MessageShape) -> bool {
    descriptor.namespace == "rbt.tf" && descriptor.name == name && descriptor.shape == shape
}

fn consumers_for(view: &InspectionView, provider: &TfProviderStatus) -> Vec<TfBindingConsumer> {
    let mut consumers = view
        .components
        .iter()
        .filter_map(|component| consumer_bindings(component, provider))
        .flatten()
        .collect::<Vec<_>>();
    consumers.sort_by(|left, right| {
        left.component
            .cmp(&right.component)
            .then_with(|| left.requirement.cmp(&right.requirement))
            .then_with(|| left.process.cmp(&right.process))
    });
    consumers
}

fn consumer_bindings(
    component: &ComponentInspection,
    provider: &TfProviderStatus,
) -> Option<Vec<TfBindingConsumer>> {
    let activation = component.snapshot.activation.as_ref()?;
    Some(
        activation
            .bindings
            .iter()
            .filter(|binding| {
                binding.capability_type == provider.capability_type
                    && binding.provider.to_string() == provider.provider_component
                    && binding.descriptor.capability_instance.to_string()
                        == provider.capability_instance
                    && binding.descriptor.service_name() == provider.physical_endpoint
                    && binding.descriptor.provider_component.to_string()
                        == provider.provider_component
                    && binding.descriptor.provider_process.to_string() == provider.provider_process
            })
            .map(|binding| TfBindingConsumer {
                component: component.component_id.to_string(),
                process: component.snapshot.process_instance.to_string(),
                activation: activation.activation_id.value(),
                connection: match component.control_connection {
                    rbt_compose::ControlConnectionStatus::Connected => "connected",
                    rbt_compose::ControlConnectionStatus::Disconnected => "disconnected",
                    rbt_compose::ControlConnectionStatus::Conflict => "conflict",
                },
                requirement: binding.requirement_name.clone(),
                capability_type: binding.capability_type.clone(),
            })
            .collect(),
    )
}

fn validate_snapshot(snapshot: &TfSnapshotInput) -> Result<(), TfStatusError> {
    validate_optional_label("consumer_name", snapshot.consumer_name.as_deref())?;
    validate_optional_label(
        "consumer_physical_identity",
        snapshot.consumer_physical_identity.as_deref(),
    )?;
    let buffer = &snapshot.buffer;
    if buffer.active_providers > MAX_PROVIDERS
        || buffer.providers.len() > MAX_PROVIDERS
        || buffer.active_frames > MAX_FRAMES
        || buffer.graph.len() > MAX_EDGES
        || buffer.stored_samples > MAX_SAMPLES
        || buffer.active_providers != buffer.providers.len()
    {
        return Err(invalid("provider, edge, frame, or sample bound exceeded"));
    }
    validate_counter("evicted_samples", buffer.evicted_samples)?;
    validate_counter("lookup_attempts", buffer.lookup_attempts)?;
    validate_counter("lookup_successes", buffer.lookup_successes)?;
    validate_counter("lookup_failures", buffer.lookup_failures)?;
    validate_counter("missing_transform_count", buffer.missing_transform_count)?;
    validate_optional_counter_leq(
        "missing_frame_count",
        buffer.missing_frame_count,
        "missing_transform_count",
        buffer.missing_transform_count,
    )?;
    validate_counter("extrapolation_count", buffer.extrapolation_count)?;
    validate_counter("clock_error_count", buffer.clock_error_count)?;
    validate_optional_counter_leq(
        "clock_mapping_failure_count",
        buffer.clock_mapping_failure_count,
        "clock_error_count",
        buffer.clock_error_count,
    )?;
    validate_counter("conflict_count", buffer.conflict_count)?;
    validate_optional_counter_leq(
        "authority_conflict_count",
        buffer.authority_conflict_count,
        "conflict_count",
        buffer.conflict_count,
    )?;

    let mut authorities = BTreeMap::new();
    let mut physical_authorities = BTreeSet::new();
    let mut static_edges = 0_usize;
    let mut dynamic_edges = 0_usize;
    let mut stored_samples = 0_usize;
    for provider in &buffer.providers {
        validate_authority(&provider.authority)?;
        if !physical_authorities.insert(&provider.authority.physical) {
            return Err(invalid("provider physical identities must be unique"));
        }
        if authorities
            .insert(
                provider.authority.slot.clone(),
                provider.authority.physical.clone(),
            )
            .is_some()
        {
            return Err(invalid("provider slots must be unique"));
        }
        if let Some(revision) = provider.static_revision {
            validate_counter("static_revision", revision)?;
        }
        if let Some(revision) = provider.highest_observed_revision {
            validate_counter("highest_observed_revision", revision)?;
        }
        validate_age("snapshot_age_ns", provider.snapshot_age_ns)?;
        validate_age("dynamic_update_age_ns", provider.dynamic_update_age_ns)?;
        validate_optional_counter_leq(
            "missing_frame_count",
            provider.missing_frame_count,
            "missing_transform_count",
            provider.missing_transform_count,
        )?;
        validate_optional_counter_leq(
            "clock_mapping_failure_count",
            provider.clock_mapping_failure_count,
            "clock_error_count",
            provider.clock_error_count,
        )?;
        validate_optional_counter_leq(
            "authority_conflict_count",
            provider.authority_conflict_count,
            "conflict_count",
            provider.conflict_count,
        )?;
        if provider.active_static_edges > MAX_EDGES
            || provider.active_dynamic_edges > MAX_EDGES
            || provider.stored_samples > MAX_SAMPLES
        {
            return Err(invalid("provider edge or sample bound exceeded"));
        }
        static_edges = static_edges
            .checked_add(provider.active_static_edges)
            .ok_or_else(|| invalid("edge count overflow"))?;
        dynamic_edges = dynamic_edges
            .checked_add(provider.active_dynamic_edges)
            .ok_or_else(|| invalid("edge count overflow"))?;
        stored_samples = stored_samples
            .checked_add(provider.stored_samples)
            .ok_or_else(|| invalid("sample count overflow"))?;
        for (name, value) in [
            ("evicted_samples", provider.evicted_samples),
            (
                "buffer_observed_lookup_attempts",
                provider.buffer_observed_lookup_attempts,
            ),
            (
                "buffer_observed_lookup_successes",
                provider.buffer_observed_lookup_successes,
            ),
            (
                "buffer_observed_lookup_failures",
                provider.buffer_observed_lookup_failures,
            ),
            ("missing_transform_count", provider.missing_transform_count),
            ("extrapolation_count", provider.extrapolation_count),
            ("clock_error_count", provider.clock_error_count),
            ("conflict_count", provider.conflict_count),
            ("resync_attempts", provider.resync_attempts),
            ("resync_failures", provider.resync_failures),
        ] {
            validate_counter(name, value)?;
        }
    }
    if static_edges
        .checked_add(dynamic_edges)
        .is_none_or(|count| count > MAX_EDGES)
        || static_edges != buffer.static_edges
        || dynamic_edges != buffer.dynamic_edges
        || stored_samples != buffer.stored_samples
    {
        return Err(invalid("buffer totals do not match provider observations"));
    }

    let mut frames = BTreeSet::new();
    for edge in &buffer.graph {
        validate_frame(&edge.parent)?;
        validate_frame(&edge.child)?;
        if edge.parent == edge.child {
            return Err(invalid(
                "graph edges cannot have identical parent and child frames",
            ));
        }
        validate_authority(&edge.authority)?;
        if authorities.get(&edge.authority.slot) != Some(&edge.authority.physical) {
            return Err(invalid(
                "graph edge authority does not match a provider observation",
            ));
        }
        if edge.is_static != edge.revision.is_some()
            || edge.is_static == edge.transform_epoch.is_some()
            || (edge.is_static && edge.stored_samples != 0)
            || edge.stored_samples > MAX_SAMPLES
            || edge.conflicting_claims > MAX_EDGES
        {
            return Err(invalid("graph edge state or counters are inconsistent"));
        }
        if let Some(revision) = edge.revision {
            validate_counter("graph revision", revision)?;
        }
        if let Some(epoch) = edge.transform_epoch {
            validate_counter("transform_epoch", epoch)?;
        }
        frames.insert(edge.parent.clone());
        frames.insert(edge.child.clone());
    }
    if frames.len() > MAX_FRAMES || frames.len() != buffer.active_frames {
        return Err(invalid(
            "active frame count does not match graph observations",
        ));
    }
    Ok(())
}

fn validate_authority(authority: &TfAuthority) -> Result<(), TfStatusError> {
    validate_frame(&authority.slot)?;
    let physical = uuid::Uuid::parse_str(&authority.physical)
        .map_err(|_| invalid("provider physical identity must be a UUID"))?;
    if physical.is_nil() {
        return Err(invalid("provider physical identity must not be nil"));
    }
    if physical.to_string() != authority.physical {
        return Err(invalid(
            "provider physical identity must use canonical UUID spelling",
        ));
    }
    Ok(())
}

fn validate_frame(frame: &str) -> Result<(), TfStatusError> {
    rbt_tf::FrameId::new(frame)
        .map(|_| ())
        .map_err(|_| invalid("invalid TF frame identifier"))
}

fn validate_optional_label(name: &str, value: Option<&str>) -> Result<(), TfStatusError> {
    if value.is_some_and(|value| {
        value.is_empty() || value.len() > 256 || value.chars().any(char::is_control)
    }) {
        return Err(invalid(format!(
            "{name} must be nonempty, at most 256 bytes, and contain no control characters"
        )));
    }
    Ok(())
}

fn validate_counter(name: &str, value: u64) -> Result<(), TfStatusError> {
    if value > MAX_COUNTER {
        return Err(invalid(format!(
            "{name} exceeds the reasonable counter limit"
        )));
    }
    Ok(())
}

fn validate_optional_counter(name: &str, value: Option<u64>) -> Result<(), TfStatusError> {
    if let Some(value) = value {
        validate_counter(name, value)?;
    }
    Ok(())
}

fn validate_optional_counter_leq(
    name: &str,
    value: Option<u64>,
    aggregate_name: &str,
    aggregate: u64,
) -> Result<(), TfStatusError> {
    validate_optional_counter(name, value)?;
    if value.is_some_and(|value| value > aggregate) {
        return Err(invalid(format!("{name} exceeds {aggregate_name}")));
    }
    Ok(())
}

fn validate_age(name: &str, value: Option<u64>) -> Result<(), TfStatusError> {
    if value.is_some_and(|value| value > MAX_AGE_NS) {
        return Err(invalid(format!("{name} exceeds the reasonable age limit")));
    }
    Ok(())
}

fn observation_freshness(observed_at: Option<Timestamp>) -> TfObservationFreshness {
    let unavailable = |reason_code: &str| TfObservationFreshness {
        status: "unavailable",
        reason_code: reason_code.to_owned(),
        age_ns: None,
        observed_at,
    };
    let Some(observed_at) = observed_at else {
        return unavailable("observation_timestamp_missing");
    };
    let clock = match HostClock::new() {
        Ok(clock) => clock,
        Err(_) => return unavailable("host_monotonic_clock_unavailable"),
    };
    let now = match clock.monotonic_now() {
        Ok(now) => now,
        Err(_) => return unavailable("host_monotonic_clock_unavailable"),
    };
    if observed_at.clock().checked_compatible(now.clock()).is_err() {
        return unavailable("unrelated_clock_domain");
    }
    match now.checked_duration_since(&observed_at) {
        Ok(age) => {
            let Some(age_ns) = i64::try_from(age.as_nanos()).ok() else {
                return unavailable("observation_age_out_of_range");
            };
            TfObservationFreshness {
                status: if age.as_nanos() <= u128::from(FRESHNESS_LIMIT_NS) {
                    "fresh"
                } else {
                    "stale"
                },
                reason_code: if age.as_nanos() <= u128::from(FRESHNESS_LIMIT_NS) {
                    "within_freshness_limit"
                } else {
                    "observation_older_than_2_5_seconds"
                }
                .to_owned(),
                age_ns: Some(age_ns),
                observed_at: Some(observed_at),
            }
        }
        Err(TimeError::NegativeDuration) => unavailable("observation_timestamp_is_in_the_future"),
        Err(_) => unavailable("observation_timestamp_unavailable"),
    }
}

fn invalid(message: impl Into<String>) -> TfStatusError {
    TfStatusError::Invalid(message.into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use rbt_compose::{
        CapabilityInspection, CapabilityTransportInspection, ComposerPhase,
        ControlConnectionStatus, InspectionView,
    };
    use rbt_core::{
        ActivationId, CapabilityDescriptor, CapabilityInstanceId, ComponentId, ComponentState,
        ComposerEpoch, ProcessInstanceId, RequirementSpec, ResolvedBinding, Revision,
    };
    use rbt_time::{ClockIdentity, ClockKind, ClockSourceId};
    use rbt_types::{
        DynamicTransform, RpcDescriptor, StaticSnapshot, StaticSnapshotRequest, StaticUpdate,
        StreamDescriptor, StreamProfile,
    };
    use std::time::{SystemTime, UNIX_EPOCH};

    fn empty_view() -> InspectionView {
        let epoch = ComposerEpoch::new();
        InspectionView {
            epoch,
            revision: Revision::initial(epoch),
            phase: ComposerPhase::Running,
            components: Vec::new(),
            capability_topology: Vec::new(),
            process_conflicts: Vec::new(),
        }
    }

    fn stream_capability(
        message_type: MessageTypeDescriptor,
        instance: CapabilityInstanceId,
    ) -> CapabilityInspection {
        let provider = ComponentId::from("tf-provider");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::new(7);
        let descriptor = StreamDescriptor::new(
            "physical-endpoint",
            message_type.clone(),
            StreamProfile::Control,
        );
        CapabilityInspection {
            capability_type: format!("tf.role.{}", message_type.name),
            capability_instance: instance,
            provider_component: provider,
            provider_process: process,
            activation,
            physical_endpoint: descriptor.service_name,
            transport: CapabilityTransportInspection::Stream {
                message_type,
                profile: StreamProfile::Control,
                readiness: descriptor.transport.readiness,
                queue_depth: 4,
                history_size: None,
                safe_overflow: false,
                resources: descriptor.resources,
                pressure: None,
                pressure_freshness: None,
                subscriber_observations: Vec::new(),
                assessments: Vec::new(),
            },
        }
    }

    fn snapshot_value(observed_at: Option<Timestamp>) -> TfSnapshotInput {
        TfSnapshotInput {
            observed_at,
            consumer_name: Some("local-consumer".to_owned()),
            consumer_physical_identity: Some("consumer-instance-a".to_owned()),
            buffer: TfBufferObservation {
                active_providers: 1,
                active_frames: 2,
                static_edges: 0,
                dynamic_edges: 1,
                stored_samples: 1,
                evicted_samples: 0,
                lookup_attempts: 30,
                lookup_successes: 0,
                lookup_failures: 30,
                missing_transform_count: 10,
                missing_frame_count: Some(0),
                extrapolation_count: 0,
                clock_error_count: 10,
                clock_mapping_failure_count: Some(2),
                conflict_count: 10,
                authority_conflict_count: Some(3),
                providers: vec![TfProviderObservation {
                    authority: TfAuthority {
                        slot: "moving".to_owned(),
                        physical: uuid::Uuid::new_v4().to_string(),
                    },
                    static_revision: None,
                    highest_observed_revision: None,
                    static_tracked: false,
                    static_stale: false,
                    snapshot_age_ns: None,
                    snapshot_fresh: None,
                    dynamic_update_age_ns: Some(1_000_000),
                    dynamic_fresh: true,
                    active_static_edges: 0,
                    active_dynamic_edges: 1,
                    stored_samples: 1,
                    evicted_samples: 0,
                    buffer_observed_lookup_attempts: 30,
                    buffer_observed_lookup_successes: 0,
                    buffer_observed_lookup_failures: 30,
                    missing_transform_count: 10,
                    missing_frame_count: Some(0),
                    extrapolation_count: 0,
                    clock_error_count: 10,
                    clock_mapping_failure_count: Some(2),
                    conflict_count: 10,
                    authority_conflict_count: Some(3),
                    resync_attempts: 0,
                    resync_failures: 0,
                }],
                graph: vec![TfGraphEdgeObservation {
                    parent: "world".to_owned(),
                    child: "base".to_owned(),
                    authority: TfAuthority {
                        slot: "moving".to_owned(),
                        physical: String::new(),
                    },
                    is_static: false,
                    revision: None,
                    transform_epoch: Some(1),
                    stored_samples: 1,
                    conflicting_claims: 1,
                }],
            },
        }
    }

    fn current_monotonic() -> Timestamp {
        HostClock::new()
            .expect("host monotonic clock is available")
            .monotonic_now()
            .expect("host monotonic reading is available")
    }

    fn write_snapshot_value(mut input: serde_json::Value) -> std::path::PathBuf {
        let physical = input["buffer"]["providers"][0]["authority"]["physical"]
            .as_str()
            .unwrap()
            .to_owned();
        input["buffer"]["graph"][0]["authority"]["physical"] = physical.into();
        let path = std::env::temp_dir().join(format!(
            "rbt-tf-snapshot-{}-{}.json",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::write(&path, serde_json::to_vec(&input).unwrap()).unwrap();
        path
    }

    fn write_snapshot(input: &TfSnapshotInput) -> std::path::PathBuf {
        write_snapshot_value(serde_json::to_value(input).unwrap())
    }

    #[test]
    fn composer_without_tf_providers_reports_unknown_graph_and_null_metrics() {
        let report = from_inspection(&empty_view());
        assert!(report.providers.is_empty());
        assert_eq!(report.graph_scope, "partial");
        assert_eq!(report.graph.availability, "unavailable");
        assert_eq!(report.local_lookup_metrics.availability, "unavailable");
        let json: serde_json::Value = serde_json::to_value(report).unwrap();
        assert!(json["graph"]["edges"].is_null());
        assert!(json["local_lookup_metrics"]["attempts"].is_null());
        assert!(json["local_lookup_metrics"]["missing_frame_count"].is_null());
        assert!(json["local_lookup_metrics"]["clock_mapping_failure_count"].is_null());
        assert!(json["local_lookup_metrics"]["authority_conflict_count"].is_null());
    }

    #[test]
    fn filters_tf_providers_by_namespace_name_shape_and_transport() {
        let mut view = empty_view();
        view.capability_topology.push(stream_capability(
            DynamicTransform::descriptor(),
            CapabilityInstanceId::new(),
        ));
        view.capability_topology.push(stream_capability(
            StaticUpdate::descriptor(),
            CapabilityInstanceId::new(),
        ));
        view.capability_topology.push(stream_capability(
            rbt_types::Image::descriptor(),
            CapabilityInstanceId::new(),
        ));
        let request = StaticSnapshotRequest::descriptor();
        let response = StaticSnapshot::descriptor();
        let provider = ComponentId::from("tf-static-provider");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::new(1);
        let rpc = RpcDescriptor::new("snapshot-endpoint", request.clone(), response.clone());
        view.capability_topology.push(CapabilityInspection {
            capability_type: "static-snapshot".to_owned(),
            capability_instance: CapabilityInstanceId::new(),
            provider_component: provider,
            provider_process: process,
            activation,
            physical_endpoint: rpc.service_name.clone(),
            transport: CapabilityTransportInspection::Rpc {
                request_type: request,
                response_type: response,
                readiness: rpc.transport.readiness,
                resources: rpc.resources,
                server_pressure: None,
                server_pressure_freshness: None,
                client_observations: Vec::new(),
            },
        });
        let report = from_inspection(&view);
        assert_eq!(report.providers.len(), 3);
        assert!(
            report.providers.iter().any(|provider| {
                provider.descriptors[0].message_type.name == "DynamicTransform"
            })
        );
        assert!(
            report
                .providers
                .iter()
                .any(|provider| { provider.descriptors[0].message_type.name == "StaticUpdate" })
        );
        assert!(report.providers.iter().any(|provider| {
            provider.transport_kind == "rpc"
                && provider.descriptors[1].message_type.name == "StaticSnapshot"
        }));
    }

    #[test]
    fn composer_binding_consumers_match_physical_capability_identity() {
        let mut view = empty_view();
        let instance = CapabilityInstanceId::new();
        let capability = stream_capability(DynamicTransform::descriptor(), instance);
        let provider_component = ComponentId::from("tf-provider");
        let process = match &capability.transport {
            CapabilityTransportInspection::Stream { .. } => capability.provider_process,
            _ => unreachable!(),
        };
        let activation = capability.activation;
        let endpoint = StreamDescriptor::new(
            "physical-endpoint",
            DynamicTransform::descriptor(),
            StreamProfile::Control,
        );
        let descriptor = CapabilityDescriptor::new_stream(
            instance,
            provider_component.clone(),
            process,
            activation,
            endpoint,
        );
        let binding = ResolvedBinding::new(
            "world_transform",
            capability.capability_type.clone(),
            provider_component,
            descriptor,
        );
        let consumer_component = ComponentId::from("localizer");
        let consumer_process = ProcessInstanceId::new();
        let consumer_activation = ActivationId::new(12);
        let snapshot = rbt_compose::ComponentSnapshot::new(
            consumer_component.clone(),
            consumer_process,
            ComponentState::Active,
            Some(rbt_compose::ActivationSnapshot::new(
                consumer_activation,
                [binding],
            )),
            [RequirementSpec::new(
                "world_transform",
                capability.capability_type.clone(),
            )],
            Vec::new(),
        );
        view.components.push(ComponentInspection {
            component_id: consumer_component,
            snapshot,
            snapshot_received: true,
            control_connection: ControlConnectionStatus::Connected,
            pending: None,
            last_active: None,
        });
        view.capability_topology.push(capability);
        let report = from_inspection(&view);
        assert_eq!(report.providers[0].consumers.len(), 1);
        assert_eq!(report.providers[0].consumers[0].component, "localizer");
        assert_eq!(
            report.providers[0].consumers[0].requirement,
            "world_transform"
        );
    }

    #[test]
    fn local_snapshot_reports_zero_as_measured_and_preserves_conflicts() {
        let mut input = snapshot_value(Some(current_monotonic()));
        let physical = input.buffer.providers[0].authority.physical.clone();
        input.buffer.graph[0].authority.physical = physical;
        let path = write_snapshot(&input);
        let report = from_snapshot_file(&path).unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(report.graph.availability, "available");
        assert_eq!(
            report.graph.edges.as_ref().unwrap()[0].conflicting_claims,
            1
        );
        assert_eq!(report.local_lookup_metrics.attempts, Some(30));
        assert_eq!(report.local_lookup_metrics.missing_frame_count, Some(0));
        assert_eq!(
            report.local_lookup_metrics.clock_mapping_failure_count,
            Some(2)
        );
        assert_eq!(
            report.local_lookup_metrics.authority_conflict_count,
            Some(3)
        );
        assert_eq!(
            report.local_observation.as_ref().unwrap().freshness.status,
            "fresh"
        );
        let human = format_human(&report, false);
        let json: serde_json::Value = serde_json::to_value(report).unwrap();
        assert_eq!(json["local_lookup_metrics"]["attempts"], 30);
        assert_eq!(json["local_lookup_metrics"]["missing_frame_count"], 0);
        assert_eq!(
            json["local_lookup_metrics"]["clock_mapping_failure_count"],
            2
        );
        assert_eq!(json["local_lookup_metrics"]["authority_conflict_count"], 3);
        assert_eq!(
            json["local_observation"]["buffer"]["missing_frame_count"],
            0
        );
        assert_eq!(
            json["local_observation"]["buffer"]["clock_mapping_failure_count"],
            2
        );
        assert_eq!(
            json["local_observation"]["buffer"]["authority_conflict_count"],
            3
        );
        assert_eq!(
            json["local_observation"]["buffer"]["providers"][0]["missing_frame_count"],
            0
        );
        assert_eq!(
            json["local_observation"]["buffer"]["providers"][0]["clock_mapping_failure_count"],
            2
        );
        assert_eq!(
            json["local_observation"]["buffer"]["providers"][0]["authority_conflict_count"],
            3
        );
        assert!(human.contains("missing_frame=0"));
        assert!(human.contains("clock_mapping_failure=2"));
        assert!(human.contains("authority_conflict=3"));

        let mut legacy = serde_json::to_value(snapshot_value(Some(current_monotonic()))).unwrap();
        let buffer = legacy["buffer"].as_object_mut().unwrap();
        buffer.remove("missing_frame_count");
        buffer.remove("clock_mapping_failure_count");
        buffer.remove("authority_conflict_count");
        let provider = legacy["buffer"]["providers"][0].as_object_mut().unwrap();
        provider.remove("missing_frame_count");
        provider.remove("clock_mapping_failure_count");
        provider.remove("authority_conflict_count");
        let path = write_snapshot_value(legacy);
        let legacy_report = from_snapshot_file(&path).unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(legacy_report.local_lookup_metrics.missing_frame_count, None);
        assert_eq!(
            legacy_report
                .local_lookup_metrics
                .clock_mapping_failure_count,
            None
        );
        assert_eq!(
            legacy_report.local_lookup_metrics.authority_conflict_count,
            None
        );
        let legacy_json = serde_json::to_value(legacy_report).unwrap();
        assert!(legacy_json["local_lookup_metrics"]["missing_frame_count"].is_null());
        assert!(legacy_json["local_lookup_metrics"]["clock_mapping_failure_count"].is_null());
        assert!(legacy_json["local_lookup_metrics"]["authority_conflict_count"].is_null());
        assert!(
            legacy_json["local_observation"]["buffer"]["providers"][0]["missing_frame_count"]
                .is_null()
        );
        assert!(legacy_json["local_observation"]["buffer"]["providers"][0]
            ["clock_mapping_failure_count"]
            .is_null());
        assert!(
            legacy_json["local_observation"]["buffer"]["providers"][0]["authority_conflict_count"]
                .is_null()
        );

        let mut oversized = snapshot_value(Some(current_monotonic()));
        oversized.buffer.authority_conflict_count = Some(MAX_COUNTER + 1);
        assert!(validate_snapshot(&oversized).is_err());

        let mut inconsistent = snapshot_value(Some(current_monotonic()));
        inconsistent.buffer.missing_transform_count = 0;
        assert!(validate_snapshot(&inconsistent).is_err());
        let mut inconsistent = snapshot_value(Some(current_monotonic()));
        inconsistent.buffer.clock_error_count = 0;
        assert!(validate_snapshot(&inconsistent).is_err());
        let mut inconsistent = snapshot_value(Some(current_monotonic()));
        inconsistent.buffer.conflict_count = 0;
        assert!(validate_snapshot(&inconsistent).is_err());
        let mut inconsistent = snapshot_value(Some(current_monotonic()));
        inconsistent.buffer.providers[0].missing_transform_count = 0;
        assert!(validate_snapshot(&inconsistent).is_err());
        let mut inconsistent = snapshot_value(Some(current_monotonic()));
        inconsistent.buffer.providers[0].clock_error_count = 0;
        assert!(validate_snapshot(&inconsistent).is_err());
        let mut inconsistent = snapshot_value(Some(current_monotonic()));
        inconsistent.buffer.providers[0].conflict_count = 0;
        assert!(validate_snapshot(&inconsistent).is_err());
    }

    #[test]
    fn stale_unrelated_and_missing_snapshot_times_are_explicit() {
        let mut input = snapshot_value(Some(current_monotonic()));
        input.observed_at = input
            .observed_at
            .map(|stamp| stamp.checked_add_nanoseconds(-3_000_000_000).unwrap());
        let physical = input.buffer.providers[0].authority.physical.clone();
        input.buffer.graph[0].authority.physical = physical;
        let path = write_snapshot(&input);
        let stale = from_snapshot_file(&path).unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(stale.local_observation.unwrap().freshness.status, "stale");

        let mut unrelated = snapshot_value(Some(current_monotonic()));
        unrelated.observed_at = Some(Timestamp::new(
            ClockIdentity::new(
                ClockKind::Device,
                ClockSourceId::from_device_id([1; 16]).unwrap(),
                1,
            )
            .unwrap(),
            0,
        ));
        let physical = unrelated.buffer.providers[0].authority.physical.clone();
        unrelated.buffer.graph[0].authority.physical = physical;
        let path = write_snapshot(&unrelated);
        let wrong_clock = from_snapshot_file(&path).unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(
            wrong_clock.local_observation.unwrap().freshness.reason_code,
            "unrelated_clock_domain"
        );

        let mut missing = snapshot_value(None);
        let physical = missing.buffer.providers[0].authority.physical.clone();
        missing.buffer.graph[0].authority.physical = physical;
        let path = write_snapshot(&missing);
        let missing_time = from_snapshot_file(&path).unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(
            missing_time.local_observation.unwrap().freshness.status,
            "unavailable"
        );
    }

    #[test]
    fn impossible_local_authority_identities_are_rejected() {
        let mut input = snapshot_value(Some(current_monotonic()));
        input.buffer.providers[0].authority.physical = uuid::Uuid::nil().to_string();
        assert!(validate_snapshot(&input).is_err());

        let mut input = snapshot_value(Some(current_monotonic()));
        let mut duplicate = input.buffer.providers[0].clone();
        duplicate.authority.slot = "another_slot".to_owned();
        input.buffer.providers.push(duplicate);
        input.buffer.active_providers += 1;
        let error = validate_snapshot(&input).unwrap_err().to_string();
        assert!(
            error.contains("physical identities must be unique"),
            "{error}"
        );
    }

    #[test]
    fn oversized_or_corrupt_snapshot_is_rejected() {
        let path = std::env::temp_dir().join(format!("rbt-tf-corrupt-{}.json", std::process::id()));
        std::fs::write(&path, vec![b' '; MAX_SNAPSHOT_BYTES as usize + 1]).unwrap();
        assert!(matches!(
            from_snapshot_file(&path),
            Err(TfStatusError::Invalid(_))
        ));
        std::fs::write(&path, b"{\"unexpected\":true}").unwrap();
        assert!(from_snapshot_file(&path).is_err());
        std::fs::remove_file(path).unwrap();
    }
}

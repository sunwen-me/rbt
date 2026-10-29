use std::{
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use rbt_core::{
    ActivationId, CapabilityInstanceId, RpcClientMetrics, RpcPressure, RpcPressureSnapshotSource,
    RpcServerMetrics,
};
use rbt_types::RpcResourceConfig;

fn increment_and_get(value: &AtomicU64) -> u64 {
    let previous = value
        .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
            Some(current.saturating_add(1))
        })
        .unwrap_or_else(|current| current);
    previous.saturating_add(1)
}

fn increment(value: &AtomicU64) {
    let _ = increment_and_get(value);
}

fn decrement(value: &AtomicU64) {
    let _ = value.fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
        Some(current.saturating_sub(1))
    });
}

fn add(value: &AtomicU64, amount: u64) {
    let _ = value.fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
        Some(current.saturating_add(amount))
    });
}

fn update_max(value: &AtomicU64, candidate: u64) {
    let mut current = value.load(Ordering::Acquire);
    while candidate > current {
        match value.compare_exchange_weak(current, candidate, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => return,
            Err(observed) => current = observed,
        }
    }
}

fn timestamp_unix_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
        .try_into()
        .unwrap_or(u64::MAX)
}

fn duration_ns(duration: Duration) -> u64 {
    duration.as_nanos().try_into().unwrap_or(u64::MAX)
}

#[derive(Clone, Copy)]
pub(crate) enum ClientTerminal {
    Completed,
    Failed,
    TimedOut,
    Cancelled,
    ActivationExpired,
}

#[derive(Default)]
struct ClientCounters {
    calls_attempted: AtomicU64,
    calls_started: AtomicU64,
    calls_completed: AtomicU64,
    calls_failed: AtomicU64,
    calls_timed_out: AtomicU64,
    calls_cancelled: AtomicU64,
    calls_activation_expired: AtomicU64,
    in_flight_current: AtomicU64,
    in_flight_high_water: AtomicU64,
    response_guards_current: AtomicU64,
    response_guards_high_water: AtomicU64,
    capacity_rejections: AtomicU64,
    server_capacity_rejections: AtomicU64,
    server_unavailable: AtomicU64,
    request_loan_failures: AtomicU64,
    response_receive_failures: AtomicU64,
    call_duration_count: AtomicU64,
    call_duration_total_ns: AtomicU64,
    call_duration_max_ns: AtomicU64,
}

pub(crate) struct ClientPressureState {
    service_name: String,
    resources: RpcResourceConfig,
    capability_instance: Option<CapabilityInstanceId>,
    activation: Option<ActivationId>,
    counters: ClientCounters,
}

impl ClientPressureState {
    pub(crate) fn new(
        service_name: String,
        resources: RpcResourceConfig,
        capability_instance: Option<CapabilityInstanceId>,
        activation: Option<ActivationId>,
    ) -> Arc<Self> {
        Arc::new(Self {
            service_name,
            resources,
            capability_instance,
            activation,
            counters: ClientCounters::default(),
        })
    }

    pub(crate) fn attempted(&self) {
        increment(&self.counters.calls_attempted);
    }

    pub(crate) fn request_loan_failed(&self) {
        increment(&self.counters.request_loan_failures);
    }

    pub(crate) fn response_receive_failed(&self) {
        increment(&self.counters.response_receive_failures);
    }

    pub(crate) fn client_capacity_rejected(&self) {
        increment(&self.counters.capacity_rejections);
        increment(&self.counters.calls_failed);
    }

    pub(crate) fn server_capacity_rejected(&self) {
        increment(&self.counters.server_capacity_rejections);
        increment(&self.counters.calls_failed);
    }

    pub(crate) fn server_unavailable(&self) {
        increment(&self.counters.server_unavailable);
        increment(&self.counters.calls_failed);
    }

    pub(crate) fn failed_before_start(&self) {
        increment(&self.counters.calls_failed);
    }

    pub(crate) fn activation_expired_before_start(&self) {
        increment(&self.counters.calls_activation_expired);
    }

    pub(crate) fn started(self: &Arc<Self>) -> ClientCallGuard {
        increment(&self.counters.calls_started);
        let current = increment_and_get(&self.counters.in_flight_current);
        update_max(&self.counters.in_flight_high_water, current);
        ClientCallGuard {
            state: Arc::clone(self),
            started: Instant::now(),
            active: true,
        }
    }

    fn response_guard(self: &Arc<Self>) -> ClientResponseGuard {
        let current = increment_and_get(&self.counters.response_guards_current);
        update_max(&self.counters.response_guards_high_water, current);
        ClientResponseGuard {
            state: Arc::clone(self),
        }
    }

    pub(crate) fn snapshot(&self) -> RpcPressure {
        self.pressure_snapshot()
    }
}

impl RpcPressureSnapshotSource for ClientPressureState {
    fn pressure_snapshot(&self) -> RpcPressure {
        let counters = &self.counters;
        RpcPressure {
            service_name: self.service_name.clone(),
            capability_instance: self.capability_instance,
            resources: self.resources,
            client_activation: self.activation,
            client: Some(RpcClientMetrics {
                calls_attempted: counters.calls_attempted.load(Ordering::Acquire),
                calls_started: counters.calls_started.load(Ordering::Acquire),
                calls_completed: counters.calls_completed.load(Ordering::Acquire),
                calls_failed: counters.calls_failed.load(Ordering::Acquire),
                calls_timed_out: counters.calls_timed_out.load(Ordering::Acquire),
                calls_cancelled: counters.calls_cancelled.load(Ordering::Acquire),
                calls_activation_expired: counters.calls_activation_expired.load(Ordering::Acquire),
                in_flight_current: counters.in_flight_current.load(Ordering::Acquire),
                in_flight_high_water: counters.in_flight_high_water.load(Ordering::Acquire),
                response_guards_current: counters.response_guards_current.load(Ordering::Acquire),
                response_guards_high_water: counters
                    .response_guards_high_water
                    .load(Ordering::Acquire),
                capacity_rejections: counters.capacity_rejections.load(Ordering::Acquire),
                server_capacity_rejections: counters
                    .server_capacity_rejections
                    .load(Ordering::Acquire),
                server_unavailable: counters.server_unavailable.load(Ordering::Acquire),
                request_loan_failures: counters.request_loan_failures.load(Ordering::Acquire),
                response_receive_failures: counters
                    .response_receive_failures
                    .load(Ordering::Acquire),
                call_duration_count: counters.call_duration_count.load(Ordering::Acquire),
                call_duration_total_ns: counters.call_duration_total_ns.load(Ordering::Acquire),
                call_duration_max_ns: counters.call_duration_max_ns.load(Ordering::Acquire),
            }),
            server: None,
            snapshot_timestamp_unix_ms: timestamp_unix_ms(),
        }
    }
}

pub(crate) struct ClientCallGuard {
    state: Arc<ClientPressureState>,
    started: Instant,
    active: bool,
}

impl ClientCallGuard {
    pub(crate) fn response_receive_failed(&self) {
        self.state.response_receive_failed();
    }

    pub(crate) fn response_guard(&self) -> ClientResponseGuard {
        self.state.response_guard()
    }

    pub(crate) fn finish(&mut self, terminal: ClientTerminal) {
        if !self.active {
            return;
        }
        self.active = false;
        decrement(&self.state.counters.in_flight_current);
        match terminal {
            ClientTerminal::Completed => increment(&self.state.counters.calls_completed),
            ClientTerminal::Failed => increment(&self.state.counters.calls_failed),
            ClientTerminal::TimedOut => increment(&self.state.counters.calls_timed_out),
            ClientTerminal::Cancelled => increment(&self.state.counters.calls_cancelled),
            ClientTerminal::ActivationExpired => {
                increment(&self.state.counters.calls_activation_expired)
            }
        }
        let elapsed = duration_ns(self.started.elapsed());
        increment(&self.state.counters.call_duration_count);
        add(&self.state.counters.call_duration_total_ns, elapsed);
        update_max(&self.state.counters.call_duration_max_ns, elapsed);
    }
}

/// Keeps response ownership telemetry active for the lifetime of a native
/// response guard, including when that guard is retained through a Python
/// image view.
pub(crate) struct ClientResponseGuard {
    state: Arc<ClientPressureState>,
}

impl Drop for ClientResponseGuard {
    fn drop(&mut self) {
        decrement(&self.state.counters.response_guards_current);
    }
}

impl Drop for ClientCallGuard {
    fn drop(&mut self) {
        self.finish(ClientTerminal::Failed);
    }
}

#[derive(Default)]
struct ServerCounters {
    requests_received: AtomicU64,
    requests_current: AtomicU64,
    requests_high_water: AtomicU64,
    responses_attempted: AtomicU64,
    responses_sent: AtomicU64,
    responses_failed: AtomicU64,
    responses_abandoned: AtomicU64,
    request_borrow_failures: AtomicU64,
    response_loan_failures: AtomicU64,
}

pub(crate) struct ServerPressureState {
    service_name: String,
    resources: RpcResourceConfig,
    capability_instance: Option<CapabilityInstanceId>,
    counters: ServerCounters,
}

impl ServerPressureState {
    pub(crate) fn new(
        service_name: String,
        resources: RpcResourceConfig,
        capability_instance: Option<CapabilityInstanceId>,
    ) -> Arc<Self> {
        Arc::new(Self {
            service_name,
            resources,
            capability_instance,
            counters: ServerCounters::default(),
        })
    }

    pub(crate) fn request_received(self: &Arc<Self>) -> ServerRequestGuard {
        increment(&self.counters.requests_received);
        let current = increment_and_get(&self.counters.requests_current);
        update_max(&self.counters.requests_high_water, current);
        ServerRequestGuard {
            state: Arc::clone(self),
            active: true,
        }
    }

    pub(crate) fn request_borrow_failed(&self) {
        increment(&self.counters.request_borrow_failures);
    }

    pub(crate) fn response_attempted(&self) {
        increment(&self.counters.responses_attempted);
    }

    pub(crate) fn response_loan_failed(&self) {
        increment(&self.counters.response_loan_failures);
        increment(&self.counters.responses_failed);
    }

    pub(crate) fn response_sent(&self) {
        increment(&self.counters.responses_sent);
    }

    pub(crate) fn response_failed(&self) {
        increment(&self.counters.responses_failed);
    }

    pub(crate) fn snapshot(&self) -> RpcPressure {
        self.pressure_snapshot()
    }
}

impl RpcPressureSnapshotSource for ServerPressureState {
    fn pressure_snapshot(&self) -> RpcPressure {
        let counters = &self.counters;
        RpcPressure {
            service_name: self.service_name.clone(),
            capability_instance: self.capability_instance,
            resources: self.resources,
            client_activation: None,
            client: None,
            server: Some(RpcServerMetrics {
                requests_received: counters.requests_received.load(Ordering::Acquire),
                requests_current: counters.requests_current.load(Ordering::Acquire),
                requests_high_water: counters.requests_high_water.load(Ordering::Acquire),
                responses_attempted: counters.responses_attempted.load(Ordering::Acquire),
                responses_sent: counters.responses_sent.load(Ordering::Acquire),
                responses_failed: counters.responses_failed.load(Ordering::Acquire),
                responses_abandoned: counters.responses_abandoned.load(Ordering::Acquire),
                request_borrow_failures: counters.request_borrow_failures.load(Ordering::Acquire),
                response_loan_failures: counters.response_loan_failures.load(Ordering::Acquire),
            }),
            snapshot_timestamp_unix_ms: timestamp_unix_ms(),
        }
    }
}

pub(crate) struct ServerRequestGuard {
    state: Arc<ServerPressureState>,
    active: bool,
}

impl ServerRequestGuard {
    pub(crate) fn response_attempted(&self) {
        self.state.response_attempted();
    }

    pub(crate) fn response_loan_failed(&self) {
        self.state.response_loan_failed();
    }

    pub(crate) fn response_sent(&self) {
        self.state.response_sent();
    }

    pub(crate) fn response_failed(&self) {
        self.state.response_failed();
    }

    pub(crate) fn responded(&mut self) {
        if !self.active {
            return;
        }
        self.active = false;
        decrement(&self.state.counters.requests_current);
    }
}

impl Drop for ServerRequestGuard {
    fn drop(&mut self) {
        if self.active {
            self.active = false;
            decrement(&self.state.counters.requests_current);
            increment(&self.state.counters.responses_abandoned);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn atomic_counter_helpers_saturate_without_wrapping() {
        let value = AtomicU64::new(u64::MAX - 1);
        assert_eq!(increment_and_get(&value), u64::MAX);
        assert_eq!(increment_and_get(&value), u64::MAX);
        add(&value, 42);
        assert_eq!(value.load(Ordering::Acquire), u64::MAX);
        decrement(&value);
        assert_eq!(value.load(Ordering::Acquire), u64::MAX - 1);
    }
}

//! Typed Python owners for the native TF streams and static-snapshot RPC.

use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};

use pyo3::{
    Bound, Py, PyErr, PyRef, PyResult, Python,
    exceptions::{PyRuntimeError, PyTypeError, PyValueError},
    prelude::*,
    pyclass, pymethods,
    types::{PyAny, PyAnyMethods, PyDict, PyDictMethods, PyModule},
};
use rbt_core::{ActivationId, CapabilityInstanceId, ProcessInstanceId};
use rbt_iox2::{WaitContext, WaitProgress, WaitToken};
use rbt_tf::{Authority, TransformBuffer};
use rbt_tf_transport::{
    DrainReport, DynamicTfPublisher, DynamicTfSubscriber, StaticServeOutcome, StaticTfClient,
    StaticTfProvider, TransportError,
};
use rbt_types::{RpcDescriptor, StreamDescriptor};
use uuid::Uuid;

use crate::tf::{
    PyTfAuthority, PyTfTransformBuffer, authority_handle, parse_dynamic_record, parse_static_set,
    static_set_object, tf_error_code, tf_py_error, unsigned_value,
};

const MAX_STATIC_TRANSFORMS: usize = 4096;
const MAX_DRAIN_BUDGET: usize = 64;

struct DynamicPublisherState {
    native: Mutex<Option<DynamicTfPublisher>>,
    authority: CapabilityInstanceId,
    closed: AtomicBool,
}

#[pyclass(name = "_DynamicTfPublisher", module = "rbt._rbt")]
struct PyDynamicTfPublisher {
    state: Arc<DynamicPublisherState>,
}

#[pymethods]
impl PyDynamicTfPublisher {
    #[staticmethod]
    fn open(py: Python<'_>, descriptor_json: &str, authority: &str) -> PyResult<Self> {
        let descriptor = parse_stream_descriptor(descriptor_json)?;
        let authority = parse_capability_id(authority)?;
        let native = DynamicTfPublisher::open(&descriptor, authority)
            .map_err(|error| transport_py_error(py, error))?;
        Ok(Self {
            state: Arc::new(DynamicPublisherState {
                native: Mutex::new(Some(native)),
                authority,
                closed: AtomicBool::new(false),
            }),
        })
    }

    #[getter]
    fn authority(&self) -> String {
        self.state.authority.as_uuid().to_string()
    }

    fn publish(&self, py: Python<'_>, value: Bound<'_, PyAny>) -> PyResult<usize> {
        let record = parse_dynamic_record(py, &value)?;
        let guard = self.state.native.lock().map_err(|_| {
            transport_py_error(py, TransportError::LockPoisoned("dynamic publisher"))
        })?;
        let native = guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "dynamic TF publisher"))?;
        native
            .publish(&record)
            .map_err(|error| transport_py_error(py, error))
    }

    fn pressure_snapshot<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let guard = self.state.native.lock().map_err(|_| {
            transport_py_error(py, TransportError::LockPoisoned("dynamic publisher"))
        })?;
        let native = guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "dynamic TF publisher"))?;
        pressure_object(
            py,
            &native
                .pressure_snapshot()
                .map_err(|error| transport_py_error(py, error))?,
        )
    }

    fn close(&self) {
        close_option(&self.state.closed, &self.state.native);
    }
}

impl Drop for PyDynamicTfPublisher {
    fn drop(&mut self) {
        close_option(&self.state.closed, &self.state.native);
    }
}

struct ActiveWait {
    waiting: AtomicBool,
    wake: Mutex<Option<rbt_iox2::WaitWakeHandle>>,
}

impl ActiveWait {
    fn new() -> Self {
        Self {
            waiting: AtomicBool::new(false),
            wake: Mutex::new(None),
        }
    }

    fn cancel(&self) {
        let wake = self
            .wake
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .take();
        if let Some(wake) = wake {
            let _ = wake.wake();
        }
    }
}

struct DynamicSubscriberState {
    native: Mutex<Option<DynamicTfSubscriber>>,
    authority: Authority,
    physical_authority: CapabilityInstanceId,
    buffer: TransformBuffer,
    closed: AtomicBool,
    active_wait: ActiveWait,
}

#[pyclass(name = "_DynamicTfSubscriber", module = "rbt._rbt")]
struct PyDynamicTfSubscriber {
    state: Arc<DynamicSubscriberState>,
}

#[pymethods]
impl PyDynamicTfSubscriber {
    #[staticmethod]
    fn open(
        py: Python<'_>,
        descriptor_json: &str,
        expected_authority: &str,
        slot: &str,
        buffer: PyRef<'_, PyTfTransformBuffer>,
    ) -> PyResult<Self> {
        let descriptor = parse_stream_descriptor(descriptor_json)?;
        let physical_authority = parse_capability_id(expected_authority)?;
        let native =
            DynamicTfSubscriber::open(&descriptor, physical_authority, slot, buffer.buffer.clone())
                .map_err(|error| transport_py_error(py, error))?;
        let authority = native.authority();
        Ok(Self {
            state: Arc::new(DynamicSubscriberState {
                native: Mutex::new(Some(native)),
                authority,
                physical_authority,
                buffer: buffer.buffer.clone(),
                closed: AtomicBool::new(false),
                active_wait: ActiveWait::new(),
            }),
        })
    }

    #[getter]
    fn authority(&self, py: Python<'_>) -> PyResult<Py<PyTfAuthority>> {
        authority_handle(py, self.state.authority)
    }

    #[getter]
    fn physical_authority(&self) -> String {
        self.state.physical_authority.as_uuid().to_string()
    }

    #[pyo3(signature=(budget=None))]
    fn drain_bounded<'py>(
        &self,
        py: Python<'py>,
        budget: Option<Bound<'py, PyAny>>,
    ) -> PyResult<Bound<'py, PyAny>> {
        let budget = budget
            .map(|value| drain_budget(&value))
            .transpose()?
            .unwrap_or(64);
        let guard = self.state.native.lock().map_err(|_| {
            transport_py_error(py, TransportError::LockPoisoned("dynamic subscriber"))
        })?;
        let native = guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "dynamic TF subscriber"))?;
        let report = native
            .drain_bounded(budget)
            .map_err(|error| transport_py_error(py, error))?;
        drain_report_object(py, &report)
    }

    fn wait_and_drain<'py>(
        &self,
        py: Python<'py>,
        timeout_seconds: Bound<'py, PyAny>,
    ) -> PyResult<Bound<'py, PyAny>> {
        let timeout = timeout(&timeout_seconds, "timeout")?;
        if self.state.active_wait.waiting.swap(true, Ordering::AcqRel) {
            return Err(custom_transport_error(
                py,
                "concurrent_wait",
                "only one wait_and_drain call may block on this subscriber",
            )?);
        }
        let state = Arc::clone(&self.state);
        let result = py.detach(move || {
            let result = run_dynamic_wait(&state, timeout);
            state.active_wait.waiting.store(false, Ordering::Release);
            result
        });
        match result {
            Ok(report) => drain_report_object(py, &report),
            Err(error) => Err(transport_py_error(py, error)),
        }
    }

    fn pressure_snapshot<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let guard = self.state.native.lock().map_err(|_| {
            transport_py_error(py, TransportError::LockPoisoned("dynamic subscriber"))
        })?;
        let native = guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "dynamic TF subscriber"))?;
        pressure_object(py, &native.pressure_snapshot())
    }

    fn buffer_pressure_snapshot<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let snapshot = self
            .state
            .buffer
            .pressure_snapshot()
            .map_err(|error| tf_py_error(py, error))?;
        json_object(py, &snapshot)
    }

    fn close(&self) {
        self.state.closed.store(true, Ordering::Release);
        self.state.active_wait.cancel();
        take_option(&self.state.native);
    }
}

impl Drop for PyDynamicTfSubscriber {
    fn drop(&mut self) {
        self.state.closed.store(true, Ordering::Release);
        self.state.active_wait.cancel();
        take_option(&self.state.native);
    }
}

struct StaticProviderState {
    native: Mutex<Option<StaticTfProvider>>,
    authority: CapabilityInstanceId,
    wake: rbt_iox2::RpcServerWakeHandle,
    closed: AtomicBool,
}

#[pyclass(name = "_StaticTfProvider", module = "rbt._rbt")]
struct PyStaticTfProvider {
    state: Arc<StaticProviderState>,
}

#[pymethods]
impl PyStaticTfProvider {
    #[staticmethod]
    fn open(
        py: Python<'_>,
        update_descriptor_json: &str,
        snapshot_descriptor_json: &str,
        update_authority: &str,
        snapshot_capability: &str,
        initial: Bound<'_, PyAny>,
        maximum_transforms: Bound<'_, PyAny>,
    ) -> PyResult<Self> {
        let update_descriptor = parse_stream_descriptor(update_descriptor_json)?;
        let snapshot_descriptor = parse_rpc_descriptor(snapshot_descriptor_json)?;
        let update_authority = parse_capability_id(update_authority)?;
        let snapshot_capability = parse_capability_id(snapshot_capability)?;
        let initial = parse_static_set(py, &initial)?;
        let maximum_transforms = static_bound(&maximum_transforms)?;
        let native = StaticTfProvider::open(
            &update_descriptor,
            &snapshot_descriptor,
            update_authority,
            snapshot_capability,
            initial,
            maximum_transforms,
        )
        .map_err(|error| transport_py_error(py, error))?;
        let wake = native.wake_handle();
        Ok(Self {
            state: Arc::new(StaticProviderState {
                native: Mutex::new(Some(native)),
                authority: update_authority,
                wake,
                closed: AtomicBool::new(false),
            }),
        })
    }

    #[getter]
    fn authority(&self) -> String {
        self.state.authority.as_uuid().to_string()
    }

    #[getter]
    fn current<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let guard =
            self.state.native.lock().map_err(|_| {
                transport_py_error(py, TransportError::LockPoisoned("static provider"))
            })?;
        let native = guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "static TF provider"))?;
        let current = native
            .control()
            .current()
            .map_err(|error| transport_py_error(py, error))?;
        static_set_object(py, &current)
    }

    fn replace(&self, py: Python<'_>, replacement: Bound<'_, PyAny>) -> PyResult<()> {
        let replacement = parse_static_set(py, &replacement)?;
        let guard =
            self.state.native.lock().map_err(|_| {
                transport_py_error(py, TransportError::LockPoisoned("static provider"))
            })?;
        guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "static TF provider"))?
            .replace(replacement)
            .map_err(|error| transport_py_error(py, error))
    }

    fn serve_one(
        &self,
        py: Python<'_>,
        timeout_seconds: Bound<'_, PyAny>,
    ) -> PyResult<Option<String>> {
        let timeout = timeout(&timeout_seconds, "timeout")?;
        let state = Arc::clone(&self.state);
        let result = py.detach(move || {
            let mut guard = state
                .native
                .lock()
                .map_err(|_| TransportError::LockPoisoned("static provider"))?;
            let native = guard.as_mut().ok_or(TransportError::ActivationEnded)?;
            native.serve_one(timeout)
        });
        match result {
            Ok(StaticServeOutcome::NoRequest) => Ok(None),
            Ok(StaticServeOutcome::Served { status: 0 }) => Ok(Some("success".to_owned())),
            Ok(StaticServeOutcome::Served { status: 1 }) => {
                Ok(Some("snapshot_too_large".to_owned()))
            }
            Ok(StaticServeOutcome::Served { status: 2 }) => {
                Ok(Some("provider_unavailable".to_owned()))
            }
            Ok(StaticServeOutcome::Served { status }) => {
                Ok(Some(format!("unknown_status_{status}")))
            }
            Err(error) => Err(transport_py_error(py, error)),
        }
    }

    fn pressure_snapshot<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let guard =
            self.state.native.lock().map_err(|_| {
                transport_py_error(py, TransportError::LockPoisoned("static provider"))
            })?;
        let native = guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "static TF provider"))?;
        let update = native
            .control()
            .update_pressure_snapshot()
            .map_err(|error| transport_py_error(py, error))?;
        let snapshot = native.snapshot_rpc_pressure();
        let output = PyDict::new(py);
        output.set_item("update", pressure_object(py, &update)?)?;
        output.set_item("snapshot_rpc", pressure_object(py, &snapshot)?)?;
        Ok(output.into_any())
    }

    fn close(&self) {
        if self.state.closed.swap(true, Ordering::AcqRel) {
            return;
        }
        let _ = self.state.wake.expire();
        take_option(&self.state.native);
    }
}

impl Drop for PyStaticTfProvider {
    fn drop(&mut self) {
        if !self.state.closed.swap(true, Ordering::AcqRel) {
            let _ = self.state.wake.expire();
        }
        take_option(&self.state.native);
    }
}

struct StaticClientState {
    native: Mutex<Option<StaticTfClient>>,
    authority: Authority,
    physical_authority: CapabilityInstanceId,
    buffer: TransformBuffer,
    rpc_wake: rbt_iox2::RpcClientWakeHandle,
    snapshot_in_flight: AtomicBool,
    closed: AtomicBool,
    active_wait: ActiveWait,
}

#[pyclass(name = "_StaticTfClient", module = "rbt._rbt")]
struct PyStaticTfClient {
    state: Arc<StaticClientState>,
}

#[pymethods]
impl PyStaticTfClient {
    #[staticmethod]
    // This bridge mirrors the named arguments of the Python lifecycle binding.
    #[allow(clippy::too_many_arguments)]
    fn open(
        py: Python<'_>,
        expected_authority: &str,
        update_descriptor_json: &str,
        snapshot_descriptor_json: &str,
        snapshot_capability: &str,
        activation: Bound<'_, PyAny>,
        slot: &str,
        maximum_transforms: Bound<'_, PyAny>,
        buffer: PyRef<'_, PyTfTransformBuffer>,
    ) -> PyResult<Self> {
        let expected_authority = parse_capability_id(expected_authority)?;
        let update_descriptor = parse_stream_descriptor(update_descriptor_json)?;
        let snapshot_descriptor = parse_rpc_descriptor(snapshot_descriptor_json)?;
        let snapshot_capability = parse_capability_id(snapshot_capability)?;
        let activation = ActivationId::new(unsigned_value(&activation, "activation")?);
        let maximum_transforms = static_bound(&maximum_transforms)?;
        let buffer_clone = buffer.buffer.clone();
        let native = StaticTfClient::open(
            expected_authority,
            &update_descriptor,
            &snapshot_descriptor,
            snapshot_capability,
            activation,
            slot,
            maximum_transforms,
            buffer_clone.clone(),
        )
        .map_err(|error| transport_py_error(py, error))?;
        let authority = native.authority();
        let rpc_wake = native.wake_handle();
        Ok(Self {
            state: Arc::new(StaticClientState {
                native: Mutex::new(Some(native)),
                authority,
                physical_authority: expected_authority,
                buffer: buffer_clone,
                rpc_wake,
                snapshot_in_flight: AtomicBool::new(false),
                closed: AtomicBool::new(false),
                active_wait: ActiveWait::new(),
            }),
        })
    }

    #[getter]
    fn authority(&self, py: Python<'_>) -> PyResult<Py<PyTfAuthority>> {
        authority_handle(py, self.state.authority)
    }

    #[getter]
    fn physical_authority(&self) -> String {
        self.state.physical_authority.as_uuid().to_string()
    }

    #[getter]
    fn snapshot_in_flight(&self) -> bool {
        self.state.snapshot_in_flight.load(Ordering::Acquire)
    }

    #[pyo3(signature=(budget=None))]
    fn drain_updates<'py>(
        &self,
        py: Python<'py>,
        budget: Option<Bound<'py, PyAny>>,
    ) -> PyResult<Bound<'py, PyAny>> {
        let budget = budget
            .map(|value| drain_budget(&value))
            .transpose()?
            .unwrap_or(64);
        let guard =
            self.state.native.lock().map_err(|_| {
                transport_py_error(py, TransportError::LockPoisoned("static client"))
            })?;
        let native = guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "static TF client"))?;
        let report = native
            .drain_updates(budget)
            .map_err(|error| transport_py_error(py, error))?;
        drain_report_object(py, &report)
    }

    fn refresh_snapshot(&self, py: Python<'_>, timeout_seconds: Bound<'_, PyAny>) -> PyResult<()> {
        let timeout = timeout(&timeout_seconds, "timeout")?;
        self.begin_snapshot(py)?;
        let state = Arc::clone(&self.state);
        let result = py.detach(move || {
            if state.closed.load(Ordering::Acquire) {
                return Err(TransportError::ActivationEnded);
            }
            let guard = state
                .native
                .lock()
                .map_err(|_| TransportError::LockPoisoned("static client"))?;
            guard
                .as_ref()
                .ok_or(TransportError::ActivationEnded)?
                .refresh_snapshot(timeout)
        });
        self.state
            .snapshot_in_flight
            .store(false, Ordering::Release);
        result.map_err(|error| transport_py_error(py, error))
    }

    fn refresh_if_due(&self, py: Python<'_>, timeout_seconds: Bound<'_, PyAny>) -> PyResult<bool> {
        let timeout = timeout(&timeout_seconds, "timeout")?;
        self.begin_snapshot(py)?;
        let state = Arc::clone(&self.state);
        let result = py.detach(move || {
            if state.closed.load(Ordering::Acquire) {
                return Err(TransportError::ActivationEnded);
            }
            let guard = state
                .native
                .lock()
                .map_err(|_| TransportError::LockPoisoned("static client"))?;
            guard
                .as_ref()
                .ok_or(TransportError::ActivationEnded)?
                .refresh_if_due(timeout)
        });
        self.state
            .snapshot_in_flight
            .store(false, Ordering::Release);
        result.map_err(|error| transport_py_error(py, error))
    }

    fn wait_and_drain<'py>(
        &self,
        py: Python<'py>,
        timeout_seconds: Bound<'py, PyAny>,
    ) -> PyResult<Bound<'py, PyAny>> {
        let timeout = timeout(&timeout_seconds, "timeout")?;
        if self.state.active_wait.waiting.swap(true, Ordering::AcqRel) {
            return Err(custom_transport_error(
                py,
                "concurrent_wait",
                "only one wait_and_drain call may block on this client",
            )?);
        }
        let state = Arc::clone(&self.state);
        let result = py.detach(move || {
            let result = run_static_wait(&state, timeout);
            state.active_wait.waiting.store(false, Ordering::Release);
            result
        });
        match result {
            Ok(report) => drain_report_object(py, &report),
            Err(error) => Err(transport_py_error(py, error)),
        }
    }

    fn needs_resync(&self, py: Python<'_>) -> PyResult<bool> {
        if self.state.closed.load(Ordering::Acquire) {
            return Err(closed_error(py, "static TF client"));
        }
        self.state
            .buffer
            .needs_resync(&self.state.authority)
            .map_err(|error| tf_py_error(py, error))
    }

    fn pressure_snapshot<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let guard =
            self.state.native.lock().map_err(|_| {
                transport_py_error(py, TransportError::LockPoisoned("static client"))
            })?;
        let native = guard
            .as_ref()
            .ok_or_else(|| closed_error(py, "static TF client"))?;
        let update = native.update_pressure_snapshot();
        let snapshot = native.snapshot_rpc_pressure();
        let buffer = self
            .state
            .buffer
            .pressure_snapshot()
            .map_err(|error| tf_py_error(py, error))?;
        let output = PyDict::new(py);
        output.set_item("update", pressure_object(py, &update)?)?;
        output.set_item("snapshot_rpc", pressure_object(py, &snapshot)?)?;
        output.set_item("buffer", json_object(py, &buffer)?)?;
        Ok(output.into_any())
    }

    fn close(&self) {
        if self.state.closed.swap(true, Ordering::AcqRel) {
            return;
        }
        let _ = self.state.rpc_wake.expire();
        self.state.active_wait.cancel();
        take_option(&self.state.native);
    }

    fn begin_snapshot(&self, py: Python<'_>) -> PyResult<()> {
        if self.state.closed.load(Ordering::Acquire) {
            return Err(closed_error(py, "static TF client"));
        }
        self.state
            .snapshot_in_flight
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map(|_| ())
            .map_err(|_| {
                custom_transport_error(
                    py,
                    "snapshot_in_flight",
                    "a static snapshot request is already in flight",
                )
                .unwrap_or_else(|error| error)
            })
    }
}

impl Drop for PyStaticTfClient {
    fn drop(&mut self) {
        if !self.state.closed.swap(true, Ordering::AcqRel) {
            let _ = self.state.rpc_wake.expire();
            self.state.active_wait.cancel();
        }
        take_option(&self.state.native);
    }
}

fn run_dynamic_wait(
    state: &DynamicSubscriberState,
    timeout: Duration,
) -> Result<DrainReport, TransportError> {
    if state.closed.load(Ordering::Acquire) {
        return Err(TransportError::ActivationEnded);
    }
    let guard = state
        .native
        .lock()
        .map_err(|_| TransportError::LockPoisoned("dynamic subscriber"))?;
    let native = guard.as_ref().ok_or(TransportError::ActivationEnded)?;
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())?;
    let wake = context.wake_handle();
    {
        let mut active = state
            .active_wait
            .wake
            .lock()
            .map_err(|_| TransportError::LockPoisoned("dynamic subscriber wait"))?;
        *active = Some(wake.clone());
    }
    if state.closed.load(Ordering::Acquire) {
        let _ = wake.wake();
    }
    let source = native.event_source(WaitToken::new(1));
    let mut report = DrainReport::default();
    let result = context.run_with_timeout(&[source], timeout, |_, budget| {
        let next = native.drain_bounded(budget)?;
        let count = next.received;
        report = next;
        Ok::<_, TransportError>(if count == 0 {
            WaitProgress::Empty
        } else {
            WaitProgress::StopAfterSamples(count)
        })
    });
    state
        .active_wait
        .wake
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .take();
    result?;
    if state.closed.load(Ordering::Acquire) {
        return Err(TransportError::ActivationEnded);
    }
    Ok(report)
}

fn run_static_wait(
    state: &StaticClientState,
    timeout: Duration,
) -> Result<DrainReport, TransportError> {
    if state.closed.load(Ordering::Acquire) {
        return Err(TransportError::ActivationEnded);
    }
    let guard = state
        .native
        .lock()
        .map_err(|_| TransportError::LockPoisoned("static client"))?;
    let native = guard.as_ref().ok_or(TransportError::ActivationEnded)?;
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())?;
    let wake = context.wake_handle();
    {
        let mut active = state
            .active_wait
            .wake
            .lock()
            .map_err(|_| TransportError::LockPoisoned("static client wait"))?;
        *active = Some(wake.clone());
    }
    if state.closed.load(Ordering::Acquire) {
        let _ = wake.wake();
    }
    let source = native.event_source(WaitToken::new(1));
    let mut report = DrainReport::default();
    let result = context.run_with_timeout(&[source], timeout, |_, budget| {
        let next = native.drain_updates(budget)?;
        let count = next.received;
        report = next;
        Ok::<_, TransportError>(if count == 0 {
            WaitProgress::Empty
        } else {
            WaitProgress::StopAfterSamples(count)
        })
    });
    state
        .active_wait
        .wake
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .take();
    result?;
    if state.closed.load(Ordering::Acquire) {
        return Err(TransportError::ActivationEnded);
    }
    Ok(report)
}

fn close_option<T>(closed: &AtomicBool, native: &Mutex<Option<T>>) {
    closed.store(true, Ordering::Release);
    take_option(native);
}

fn take_option<T>(native: &Mutex<Option<T>>) {
    let endpoint = native
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .take();
    drop(endpoint);
}

fn parse_stream_descriptor(json: &str) -> PyResult<StreamDescriptor> {
    serde_json::from_str(json)
        .map_err(|error| PyValueError::new_err(format!("invalid StreamDescriptor: {error}")))
}

fn parse_rpc_descriptor(json: &str) -> PyResult<RpcDescriptor> {
    serde_json::from_str(json)
        .map_err(|error| PyValueError::new_err(format!("invalid RpcDescriptor: {error}")))
}

fn parse_capability_id(value: &str) -> PyResult<CapabilityInstanceId> {
    let value = Uuid::parse_str(value)
        .map_err(|_| PyValueError::new_err("capability identity must be a UUID"))?;
    if value.is_nil() {
        return Err(PyValueError::new_err("capability identity must be non-nil"));
    }
    Ok(CapabilityInstanceId::from_uuid(value))
}

fn drain_budget(value: &Bound<'_, PyAny>) -> PyResult<usize> {
    let value = crate::tf::unsigned_value(value, "budget")?;
    let value = usize::try_from(value).map_err(|_| PyValueError::new_err("budget is too large"))?;
    if value > MAX_DRAIN_BUDGET {
        return Err(PyValueError::new_err("drain budget must be at most 64"));
    }
    Ok(value)
}

pub(crate) fn static_bound(value: &Bound<'_, PyAny>) -> PyResult<usize> {
    let value = crate::tf::unsigned_value(value, "maximum_transforms")?;
    let value = usize::try_from(value)
        .map_err(|_| PyValueError::new_err("maximum_transforms is too large"))?;
    if !(1..=MAX_STATIC_TRANSFORMS).contains(&value) {
        return Err(PyValueError::new_err(
            "maximum_transforms must be between 1 and 4096",
        ));
    }
    Ok(value)
}

fn timeout(value: &Bound<'_, PyAny>, name: &str) -> PyResult<Duration> {
    if value.is_instance_of::<pyo3::types::PyBool>() {
        return Err(PyTypeError::new_err(format!(
            "{name} must be a finite non-negative number"
        )));
    }
    let seconds = value.extract::<f64>().map_err(|_| {
        PyTypeError::new_err(format!("{name} must be a finite non-negative number"))
    })?;
    if !seconds.is_finite() || seconds < 0.0 {
        return Err(PyValueError::new_err(format!(
            "{name} must be finite and non-negative"
        )));
    }
    Duration::try_from_secs_f64(seconds).map_err(|error| {
        PyValueError::new_err(format!("{name} is outside its supported range: {error}"))
    })
}

fn drain_report_object<'py>(py: Python<'py>, report: &DrainReport) -> PyResult<Bound<'py, PyAny>> {
    let output = PyDict::new(py);
    output.set_item("received", report.received)?;
    output.set_item("accepted", report.accepted)?;
    output.set_item("rejected", report.rejected)?;
    output.set_item(
        "failures",
        report
            .failures
            .iter()
            .map(|error| (tf_error_code(error), error.to_string()))
            .collect::<Vec<_>>(),
    )?;
    Ok(output.into_any())
}

fn pressure_object<'py, T>(py: Python<'py>, value: &T) -> PyResult<Bound<'py, PyAny>>
where
    T: serde::Serialize,
{
    json_object(py, value)
}

fn json_object<'py, T>(py: Python<'py>, value: &T) -> PyResult<Bound<'py, PyAny>>
where
    T: serde::Serialize,
{
    let encoded = serde_json::to_string(value).map_err(|error| {
        PyRuntimeError::new_err(format!("TF diagnostics encoding failed: {error}"))
    })?;
    py.import("json")?.call_method1("loads", (encoded,))
}

fn closed_error(py: Python<'_>, endpoint: &str) -> PyErr {
    custom_transport_error(py, "transport_closed", &format!("{endpoint} is closed"))
        .unwrap_or_else(|error| error)
}

fn custom_transport_error(py: Python<'_>, code: &str, message: &str) -> PyResult<PyErr> {
    let value = py
        .import("rbt.tf")?
        .getattr("TfTransportError")?
        .call1((code, message))?;
    Ok(PyErr::from_value(value))
}

pub(crate) fn transport_py_error(py: Python<'_>, error: TransportError) -> PyErr {
    match error {
        TransportError::Tf(error) => tf_py_error(py, error),
        TransportError::Rpc(error) => crate::rpc::rpc_py_error(error),
        TransportError::Stream(error) => super::stream_py_error(error),
        TransportError::Wait(error) => super::WaitError::new_err(error.to_string()),
        TransportError::Requirement(error) => {
            super::ComponentRuntimeError::new_err(error.to_string())
        }
        TransportError::PairingInvalid(message) => {
            custom_transport_error(py, "pairing_invalid", message).unwrap_or_else(|error| error)
        }
        TransportError::ServiceIdentityMismatch => custom_transport_error(
            py,
            "service_identity_mismatch",
            "service name does not match its physical capability identity",
        )
        .unwrap_or_else(|error| error),
        TransportError::InvalidStaticReplacement => custom_transport_error(
            py,
            "invalid_static_replacement",
            "static replacement must advance one revision and effective time monotonically",
        )
        .unwrap_or_else(|error| error),
        TransportError::DrainBudget { requested, maximum } => custom_transport_error(
            py,
            "drain_budget",
            &format!("drain budget {requested} exceeds maximum {maximum}"),
        )
        .unwrap_or_else(|error| error),
        TransportError::SnapshotInFlight => custom_transport_error(
            py,
            "snapshot_in_flight",
            "a static snapshot request is already in flight",
        )
        .unwrap_or_else(|error| error),
        TransportError::ActivationEnded => custom_transport_error(
            py,
            "transport_closed",
            "TF transport endpoint is closed or its activation ended",
        )
        .unwrap_or_else(|error| error),
        TransportError::LockPoisoned(name) => custom_transport_error(
            py,
            "lock_poisoned",
            &format!("TF transport state lock was poisoned: {name}"),
        )
        .unwrap_or_else(|error| error),
    }
}

pub(crate) fn add_to_module(_py: Python<'_>, module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyDynamicTfPublisher>()?;
    module.add_class::<PyDynamicTfSubscriber>()?;
    module.add_class::<PyStaticTfProvider>()?;
    module.add_class::<PyStaticTfClient>()?;
    Ok(())
}

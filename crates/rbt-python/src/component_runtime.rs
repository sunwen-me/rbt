//! PyO3 adapter for the Rust component lifecycle.
//!
//! This module deliberately contains no second lifecycle implementation.  It
//! translates Python hooks and callables into the existing `rbt-compose`
//! [`Component`], [`ActivationCtx`], and [`EffectScope`] APIs.  Python values
//! are retained only as `Py<PyAny>` handles and every call is made while the
//! GIL is attached.

use std::{
    collections::HashSet,
    fmt::{self, Display},
    panic::{AssertUnwindSafe, catch_unwind},
    sync::atomic::{AtomicBool, AtomicU64, Ordering},
    sync::{Arc, Mutex, MutexGuard},
};

use async_trait::async_trait;
use pyo3::{
    Bound, Py, PyAny, PyResult, Python,
    exceptions::PyValueError,
    prelude::{PyTracebackMethods, *},
    types::PyDict,
};
use rbt_compose::{ActivationCtx, Component};
use rbt_core::{
    ActivationId, CapabilityInstanceId, ComponentId, ProcessInstanceId, RequirementSpec,
    ResolvedBinding,
};
use rbt_iox2::{
    Image, PointCloud, RpcDescriptor, RpcResourceConfig, SliceStream, Stream, StreamProfile,
    StreamResourceConfig, WaitWakeHandle,
};
use rbt_tf_transport::TransportError;
use rbt_types::{DynamicTransform, StaticSnapshot, StaticSnapshotRequest, StaticUpdate};
use tokio::sync::oneshot;
use tokio_util::sync::CancellationToken;

use super::{
    ActivationExpiredError, ComponentActivationError, ComponentPreparationError,
    ComponentRuntimeError, ImagePublisherCell, ImageSubscriberCell, PointCloudPublisherCell,
    PointCloudSubscriberCell, PyImagePublisher, PyImageSubscriber, PyPointCloudPublisher,
    PyPointCloudSubscriber, RbtError,
    async_runtime::{
        AsyncActivationBridge, AsyncSubscriberState, AsyncTaskControl, AsyncTaskDiagnostic,
        AsyncTaskOutcome, AsyncioRuntime,
    },
    close_image_publisher, close_image_subscriber, close_pointcloud_publisher,
    close_pointcloud_subscriber,
    rpc::{
        PyRpcClient, PyRpcServer, RpcAsyncBridgeControl, RpcClientCell, RpcServerCell,
        client_async_bridge, close_client, close_server, open_client_for_activation,
        open_server_for_capability, server_async_bridge,
    },
    tf::PyTfTransformBuffer,
    tf_transport,
};

const MAX_DIAGNOSTIC_BYTES: usize = 16 * 1024;

/// A process/activation-scoped cancellation and expiry guard.
///
/// Each bound endpoint and context receives its own `Arc` clone.  Waiters
/// register their native `WaitWakeHandle`; expiring the guard marks it first,
/// then wakes every registered wait without polling or injecting an exception
/// into Python.
pub(crate) struct ActivationGuard {
    process: ProcessInstanceId,
    activation: ActivationId,
    expired: AtomicBool,
    next_wait: AtomicU64,
    waits: Mutex<Vec<(u64, WaitWakeHandle)>>,
    rpc_bridges: Mutex<Vec<Arc<dyn RpcAsyncBridgeControl>>>,
    async_bridge: Arc<AsyncActivationBridge>,
}

impl ActivationGuard {
    pub(crate) fn new(process: ProcessInstanceId, activation: ActivationId) -> Arc<Self> {
        Arc::new(Self {
            process,
            activation,
            expired: AtomicBool::new(false),
            next_wait: AtomicU64::new(1),
            waits: Mutex::new(Vec::new()),
            rpc_bridges: Mutex::new(Vec::new()),
            async_bridge: AsyncActivationBridge::new(process, activation),
        })
    }

    pub(crate) const fn process_instance(&self) -> ProcessInstanceId {
        self.process
    }

    pub(crate) const fn activation(&self) -> ActivationId {
        self.activation
    }

    pub(crate) fn is_expired(&self) -> bool {
        self.expired.load(Ordering::Acquire)
    }

    pub(crate) fn expire(&self) {
        if self.expired.swap(true, Ordering::AcqRel) {
            return;
        }
        let waits = self
            .waits
            .lock()
            .map(|mut waits| std::mem::take(&mut *waits))
            .unwrap_or_default();
        for (_, wake) in waits {
            let _ = wake.wake();
        }
        let rpc_bridges = self
            .rpc_bridges
            .lock()
            .map(|mut bridges| std::mem::take(&mut *bridges))
            .unwrap_or_default();
        for bridge in rpc_bridges {
            bridge.expire_and_stop();
        }
        self.async_bridge.stop_and_join();
    }

    pub(crate) fn register_rpc_bridge(
        &self,
        bridge: Arc<dyn RpcAsyncBridgeControl>,
    ) -> PyResult<()> {
        if self.is_expired() {
            bridge.expire_and_stop();
            return Err(ActivationExpiredError::new_err("activation has expired"));
        }
        self.rpc_bridges
            .lock()
            .map_err(|_| RbtError::new_err("RPC bridge registry lock was poisoned"))?
            .push(bridge);
        Ok(())
    }

    pub(crate) fn register_wait(self: &Arc<Self>, wake: WaitWakeHandle) -> WaitRegistration {
        let id = self.next_wait.fetch_add(1, Ordering::Relaxed);
        let should_wake = match self.waits.lock() {
            Ok(mut waits) if !self.is_expired() => {
                waits.push((id, wake.clone()));
                false
            }
            _ => true,
        };
        if should_wake {
            let _ = wake.wake();
        }
        WaitRegistration {
            guard: Arc::clone(self),
            id: Some(id),
        }
    }

    fn unregister_wait(&self, id: u64) {
        if let Ok(mut waits) = self.waits.lock() {
            waits.retain(|(candidate, _)| *candidate != id);
        }
    }

    pub(crate) fn register_async_subscriber(
        &self,
        descriptor: &rbt_core::StreamDescriptor,
    ) -> PyResult<Arc<AsyncSubscriberState>> {
        let state = AsyncSubscriberState::new(descriptor).map_err(|error| {
            RbtError::new_err(format!("failed to open async readiness listener: {error}"))
        })?;
        self.async_bridge
            .register(Arc::clone(&state))
            .map_err(RbtError::new_err)?;
        Ok(state)
    }

    pub(crate) fn async_bridge(&self) -> Arc<AsyncActivationBridge> {
        Arc::clone(&self.async_bridge)
    }

    pub(crate) fn start_async_bridge(&self) {
        self.async_bridge.start();
    }
}

/// Removes a wait handle from an activation guard when its wait returns.
pub(crate) struct WaitRegistration {
    guard: Arc<ActivationGuard>,
    id: Option<u64>,
}

impl Drop for WaitRegistration {
    fn drop(&mut self) {
        if let Some(id) = self.id.take() {
            self.guard.unregister_wait(id);
        }
    }
}

#[derive(Clone, Debug)]
struct PythonDiagnostic {
    phase: &'static str,
    class: String,
    message: String,
    traceback: String,
}

impl Display for PythonDiagnostic {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "phase={}; exception={}; message={}",
            self.phase, self.class, self.message
        )?;
        if !self.traceback.is_empty() {
            write!(formatter, "; traceback=\n{}", self.traceback)?;
        }
        Ok(())
    }
}

#[derive(Debug, thiserror::Error)]
#[error("{diagnostic}")]
struct PythonLifecycleError {
    diagnostic: PythonDiagnostic,
}

#[derive(Debug, thiserror::Error)]
#[error("{diagnostic}")]
struct PythonTaskError {
    diagnostic: PythonDiagnostic,
}

fn async_task_error(diagnostic: AsyncTaskDiagnostic) -> PythonTaskError {
    PythonTaskError {
        diagnostic: PythonDiagnostic {
            phase: "async task",
            class: diagnostic.class,
            message: diagnostic.message,
            traceback: diagnostic.traceback,
        },
    }
}

fn bounded(mut value: String) -> String {
    if value.len() <= MAX_DIAGNOSTIC_BYTES {
        return value;
    }
    value.truncate(MAX_DIAGNOSTIC_BYTES.saturating_sub("…".len()));
    value.push('…');
    value
}

fn py_string(value: PyResult<Bound<'_, PyAny>>) -> String {
    value
        .and_then(|value| value.str())
        .and_then(|value| value.to_str().map(str::to_owned))
        .unwrap_or_else(|_| "<unprintable Python exception>".to_owned())
}

fn py_diagnostic(py: Python<'_>, phase: &'static str, error: pyo3::PyErr) -> PythonDiagnostic {
    let class = error
        .get_type(py)
        .name()
        .and_then(|name| name.to_str().map(str::to_owned))
        .unwrap_or_else(|_| "BaseException".to_owned());
    let message = py_string(Ok(error.value(py).as_any().clone()));
    let traceback = error
        .traceback(py)
        .and_then(|traceback| traceback.format().ok())
        .unwrap_or_default();
    PythonDiagnostic {
        phase,
        class: bounded(class),
        message: bounded(message),
        traceback: bounded(traceback),
    }
}

fn lock_staging<'a>(
    staging: &'a Arc<Mutex<ActivationStaging>>,
) -> PyResult<MutexGuard<'a, ActivationStaging>> {
    staging
        .lock()
        .map_err(|_| RbtError::new_err("activation staging lock was poisoned"))
}

#[derive(Clone)]
struct ProvidedDraft {
    capability_type: String,
    capability_instance: rbt_core::CapabilityInstanceId,
    endpoint: ProvidedEndpoint,
}

#[derive(Clone)]
enum ProvidedEndpoint {
    Stream(rbt_core::StreamDescriptor),
    Rpc(RpcDescriptor),
}

enum StagedEndpoint {
    ImagePublisher(ImagePublisherCell),
    PointCloudPublisher(PointCloudPublisherCell),
    ImageSubscriber(ImageSubscriberCell),
    PointCloudSubscriber(PointCloudSubscriberCell),
    RpcClient(RpcClientCell),
    RpcServer(RpcServerCell),
    DynamicTfPublisher(Py<PyAny>),
    DynamicTfSubscriber(Py<PyAny>),
    StaticTfProvider(Py<PyAny>),
    StaticTfClient(Py<PyAny>),
}

impl StagedEndpoint {
    fn close(&self) {
        match self {
            Self::ImagePublisher(cell) => close_image_publisher(cell),
            Self::PointCloudPublisher(cell) => close_pointcloud_publisher(cell),
            Self::ImageSubscriber(cell) => close_image_subscriber(cell),
            Self::PointCloudSubscriber(cell) => close_pointcloud_subscriber(cell),
            Self::RpcClient(cell) => close_client(cell),
            Self::RpcServer(cell) => close_server(cell),
            Self::DynamicTfPublisher(endpoint)
            | Self::DynamicTfSubscriber(endpoint)
            | Self::StaticTfProvider(endpoint)
            | Self::StaticTfClient(endpoint) => close_tf_endpoint(endpoint),
        }
    }
}

fn close_tf_endpoint(endpoint: &Py<PyAny>) {
    Python::attach(|py| {
        let _ = endpoint.bind(py).call_method0("close");
    });
}

struct StagedTask {
    name: String,
    callable: Py<PyAny>,
    critical: bool,
    asynchronous: bool,
    async_control: Option<Arc<AsyncTaskControl>>,
}

struct ActivationStaging {
    component: ComponentId,
    process: ProcessInstanceId,
    activation: ActivationId,
    guard: Arc<ActivationGuard>,
    async_runtime: Arc<AsyncioRuntime>,
    bindings: Vec<ResolvedBinding>,
    declared_requirements: Vec<RequirementSpec>,
    endpoints: Vec<StagedEndpoint>,
    provided: Vec<ProvidedDraft>,
    tasks: Vec<StagedTask>,
    sealed: bool,
}

impl ActivationStaging {
    fn new(
        component: ComponentId,
        process: ProcessInstanceId,
        activation: ActivationId,
        guard: Arc<ActivationGuard>,
        async_runtime: Arc<AsyncioRuntime>,
        bindings: Vec<ResolvedBinding>,
        declared_requirements: Vec<RequirementSpec>,
    ) -> Self {
        Self {
            component,
            process,
            activation,
            guard,
            async_runtime,
            bindings,
            declared_requirements,
            endpoints: Vec::new(),
            provided: Vec::new(),
            tasks: Vec::new(),
            sealed: false,
        }
    }

    fn ensure_open(&self) -> PyResult<()> {
        if self.sealed {
            Err(ActivationExpiredError::new_err(
                "activation context is sealed",
            ))
        } else {
            Ok(())
        }
    }

    fn close(&mut self) {
        self.sealed = true;
        self.guard.expire();
        for endpoint in self.endpoints.drain(..).rev() {
            endpoint.close();
        }
        self.provided.clear();
        self.tasks.clear();
    }

    fn binding(&self, requirement_name: &str) -> PyResult<ResolvedBinding> {
        let requirement = self
            .declared_requirements
            .iter()
            .find(|requirement| requirement.requirement_name == requirement_name)
            .ok_or_else(|| {
                PyValueError::new_err(format!("unknown declared requirement {requirement_name:?}"))
            })?;
        self.bindings
            .iter()
            .find(|binding| {
                binding.requirement_name == requirement.requirement_name
                    && binding.capability_type == requirement.capability_type
            })
            .cloned()
            .ok_or_else(|| {
                RbtError::new_err(format!(
                    "no resolved binding for requirement {requirement_name:?}"
                ))
            })
    }
}

/// Thin native activation context exposed to Python lifecycle hooks.
#[pyclass(module = "rbt._rbt", name = "_ActivationContext")]
pub(crate) struct PyActivationContext {
    staging: Arc<Mutex<ActivationStaging>>,
    async_runtime: Arc<AsyncioRuntime>,
}

impl PyActivationContext {
    fn require_native(
        &self,
        py: Python<'_>,
        requirement_name: &str,
        max_borrowed: Option<usize>,
    ) -> PyResult<(String, Py<PyAny>, String, String, String)> {
        let (binding, guard, activation) = {
            let staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            (
                staging.binding(requirement_name)?,
                Arc::clone(&staging.guard),
                staging.activation,
            )
        };
        if let Some(descriptor) = binding.descriptor.try_rpc().cloned() {
            let cell = open_client_for_activation(
                &descriptor,
                binding.descriptor.capability_instance(),
                activation,
            )?;
            let async_bridge =
                client_async_bridge(Arc::clone(&cell), guard.process_instance(), activation);
            guard
                .register_rpc_bridge(Arc::clone(&async_bridge) as Arc<dyn RpcAsyncBridgeControl>)?;
            let descriptor_json = serde_json::to_string(&descriptor)
                .map_err(|error| RbtError::new_err(error.to_string()))?;
            let mut staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            staging
                .endpoints
                .push(StagedEndpoint::RpcClient(Arc::clone(&cell)));
            let value = Py::new(py, PyRpcClient::from_activation(cell, async_bridge))?;
            return Ok((
                "rpc".to_owned(),
                value.into_any(),
                descriptor_json,
                binding.provider.to_string(),
                binding.descriptor.capability_instance.to_string(),
            ));
        }
        let descriptor = binding.descriptor.try_stream().cloned().ok_or_else(|| {
            PyValueError::new_err(format!(
                "requirement {requirement_name:?} resolved to an unsupported capability endpoint"
            ))
        })?;
        let advertised = descriptor.resources();
        let resources = StreamResourceConfig::new(
            advertised.max_loans,
            max_borrowed.unwrap_or(advertised.max_borrowed),
        );
        let message_type = descriptor.message_type.clone();
        if message_type == rbt_types::Image::descriptor() {
            let subscriber =
                SliceStream::<Image>::subscriber_with_resources(&descriptor, resources)
                    .map_err(super::stream_py_error)?;
            let cell = Arc::new(Mutex::new(Some(subscriber)));
            let mut staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            staging
                .endpoints
                .push(StagedEndpoint::ImageSubscriber(Arc::clone(&cell)));
            let async_state = guard.register_async_subscriber(&descriptor)?;
            let value = Py::new(
                py,
                PyImageSubscriber::from_activation(cell, guard, async_state),
            )?;
            Ok((
                "image".to_owned(),
                value.into_any(),
                serde_json::to_string(&descriptor)
                    .map_err(|error| RbtError::new_err(error.to_string()))?,
                binding.provider.to_string(),
                binding.descriptor.capability_instance.to_string(),
            ))
        } else if message_type == rbt_types::PointCloud::descriptor() {
            let subscriber =
                SliceStream::<PointCloud>::subscriber_with_resources(&descriptor, resources)
                    .map_err(super::stream_py_error)?;
            let cell = Arc::new(Mutex::new(Some(subscriber)));
            let mut staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            staging
                .endpoints
                .push(StagedEndpoint::PointCloudSubscriber(Arc::clone(&cell)));
            let async_state = guard.register_async_subscriber(&descriptor)?;
            let value = Py::new(
                py,
                PyPointCloudSubscriber::from_activation(cell, guard, async_state),
            )?;
            Ok((
                "point_cloud".to_owned(),
                value.into_any(),
                serde_json::to_string(&descriptor)
                    .map_err(|error| RbtError::new_err(error.to_string()))?,
                binding.provider.to_string(),
                binding.descriptor.capability_instance.to_string(),
            ))
        } else {
            Err(PyValueError::new_err(format!(
                "unsupported required stream message type {}",
                message_type.qualified_name()
            )))
        }
    }

    fn provide_image_native(
        &self,
        py: Python<'_>,
        capability_type: &str,
        maximum_payload_bytes: usize,
        max_loans: usize,
        max_borrowed: usize,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        let guard = {
            let staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            Arc::clone(&staging.guard)
        };
        let capability_instance = rbt_core::CapabilityInstanceId::new();
        let mut descriptor = SliceStream::<Image>::descriptor(
            capability_instance.stream_service_name(),
            rbt_types::StreamProfile::Sensor,
        );
        descriptor.resources = StreamResourceConfig::new(max_loans, max_borrowed);
        let publisher = SliceStream::<Image>::publisher_with_resources(
            &descriptor,
            maximum_payload_bytes,
            descriptor.resources,
        )
        .map_err(super::stream_py_error)?;
        let descriptor_json = serde_json::to_string(&descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let cell = Arc::new(Mutex::new(Some(publisher)));
        let mut staging = lock_staging(&self.staging)?;
        staging.ensure_open()?;
        staging
            .endpoints
            .push(StagedEndpoint::ImagePublisher(Arc::clone(&cell)));
        staging.provided.push(ProvidedDraft {
            capability_type: capability_type.to_owned(),
            capability_instance,
            endpoint: ProvidedEndpoint::Stream(descriptor),
        });
        let value = Py::new(
            py,
            PyImagePublisher::from_activation(cell, maximum_payload_bytes, guard),
        )?;
        Ok((
            value.into_any(),
            descriptor_json,
            capability_instance.to_string(),
        ))
    }

    fn provide_pointcloud_native(
        &self,
        py: Python<'_>,
        capability_type: &str,
        maximum_points: usize,
        max_loans: usize,
        max_borrowed: usize,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        let guard = {
            let staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            Arc::clone(&staging.guard)
        };
        let capability_instance = rbt_core::CapabilityInstanceId::new();
        let mut descriptor = SliceStream::<PointCloud>::descriptor(
            capability_instance.stream_service_name(),
            rbt_types::StreamProfile::Sensor,
        );
        descriptor.resources = StreamResourceConfig::new(max_loans, max_borrowed);
        let publisher = SliceStream::<PointCloud>::publisher_with_resources(
            &descriptor,
            maximum_points,
            descriptor.resources,
        )
        .map_err(super::stream_py_error)?;
        let descriptor_json = serde_json::to_string(&descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let cell = Arc::new(Mutex::new(Some(publisher)));
        let mut staging = lock_staging(&self.staging)?;
        staging.ensure_open()?;
        staging
            .endpoints
            .push(StagedEndpoint::PointCloudPublisher(Arc::clone(&cell)));
        staging.provided.push(ProvidedDraft {
            capability_type: capability_type.to_owned(),
            capability_instance,
            endpoint: ProvidedEndpoint::Stream(descriptor),
        });
        let value = Py::new(
            py,
            PyPointCloudPublisher::from_activation(cell, maximum_points, guard),
        )?;
        Ok((
            value.into_any(),
            descriptor_json,
            capability_instance.to_string(),
        ))
    }

    fn provide_rpc_native(
        &self,
        py: Python<'_>,
        capability_type: &str,
        descriptor_json: &str,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        let (guard, activation) = {
            let staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            (Arc::clone(&staging.guard), staging.activation)
        };
        let mut descriptor: RpcDescriptor =
            serde_json::from_str(descriptor_json).map_err(|error| {
                PyValueError::new_err(format!("invalid RpcDescriptor JSON: {error}"))
            })?;
        let capability_instance = CapabilityInstanceId::new();
        descriptor.service_name = capability_instance.rpc_service_name();
        let descriptor_json = serde_json::to_string(&descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let cell = open_server_for_capability(&descriptor, capability_instance)?;
        let mut staging = lock_staging(&self.staging)?;
        staging.ensure_open()?;
        staging
            .endpoints
            .push(StagedEndpoint::RpcServer(Arc::clone(&cell)));
        staging.provided.push(ProvidedDraft {
            capability_type: capability_type.to_owned(),
            capability_instance,
            endpoint: ProvidedEndpoint::Rpc(descriptor),
        });
        let async_bridge =
            server_async_bridge(Arc::clone(&cell), guard.process_instance(), activation);
        guard.register_rpc_bridge(Arc::clone(&async_bridge) as Arc<dyn RpcAsyncBridgeControl>)?;
        let value = Py::new(py, PyRpcServer::from_activation(cell, async_bridge))?;
        Ok((
            value.into_any(),
            descriptor_json,
            capability_instance.to_string(),
        ))
    }

    fn provide_dynamic_tf_native(
        &self,
        py: Python<'_>,
        capability_type: &str,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        validate_tf_role(capability_type, "dynamic TF capability")?;
        let capability_instance = CapabilityInstanceId::new();
        let descriptor = Stream::<DynamicTransform>::descriptor(
            capability_instance.stream_service_name(),
            StreamProfile::Control,
        );
        let descriptor_json = serde_json::to_string(&descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let native = py
            .import("rbt._rbt")?
            .getattr("_DynamicTfPublisher")?
            .getattr("open")?
            .call1((descriptor_json.as_str(), capability_instance.to_string()))?
            .unbind();

        let mut staging = lock_staging(&self.staging)?;
        if let Err(error) = staging.ensure_open() {
            drop(staging);
            close_tf_endpoint(&native);
            return Err(error);
        }
        staging
            .endpoints
            .push(StagedEndpoint::DynamicTfPublisher(native.clone_ref(py)));
        staging.provided.push(ProvidedDraft {
            capability_type: capability_type.to_owned(),
            capability_instance,
            endpoint: ProvidedEndpoint::Stream(descriptor),
        });
        Ok((native, descriptor_json, capability_instance.to_string()))
    }

    fn provide_static_tf_native(
        &self,
        py: Python<'_>,
        update_role: &str,
        snapshot_role: &str,
        initial: Bound<'_, PyAny>,
        maximum_transforms: Bound<'_, PyAny>,
    ) -> PyResult<(Py<PyAny>, String, String, String, String, usize)> {
        validate_tf_role(update_role, "static update capability")?;
        validate_tf_role(snapshot_role, "static snapshot capability")?;
        if update_role == snapshot_role {
            return Err(tf_pairing_error(
                py,
                "static update and snapshot must use distinct capability roles",
            ));
        }
        let maximum_transforms = tf_transport::static_bound(&maximum_transforms)?;
        let update_authority = CapabilityInstanceId::new();
        let snapshot_capability = CapabilityInstanceId::new();
        if update_authority == snapshot_capability {
            return Err(tf_pairing_error(
                py,
                "static update and snapshot must use distinct capability identities",
            ));
        }
        let update_descriptor = SliceStream::<StaticUpdate>::descriptor(
            update_authority.stream_service_name(),
            StreamProfile::Control,
        );
        let rpc_resources = RpcResourceConfig {
            max_response_slice_len: maximum_transforms,
            ..RpcResourceConfig::default()
        };
        let snapshot_descriptor = RpcDescriptor::for_fixed_slice_with_resources::<
            StaticSnapshotRequest,
            StaticSnapshot,
        >(snapshot_capability.rpc_service_name(), rpc_resources);
        let update_json = serde_json::to_string(&update_descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let snapshot_json = serde_json::to_string(&snapshot_descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let native = py
            .import("rbt._rbt")?
            .getattr("_StaticTfProvider")?
            .getattr("open")?
            .call1((
                update_json.as_str(),
                snapshot_json.as_str(),
                update_authority.to_string(),
                snapshot_capability.to_string(),
                initial,
                maximum_transforms,
            ))?
            .unbind();

        let mut staging = lock_staging(&self.staging)?;
        if let Err(error) = staging.ensure_open() {
            drop(staging);
            close_tf_endpoint(&native);
            return Err(error);
        }
        staging
            .endpoints
            .push(StagedEndpoint::StaticTfProvider(native.clone_ref(py)));
        staging.provided.push(ProvidedDraft {
            capability_type: update_role.to_owned(),
            capability_instance: update_authority,
            endpoint: ProvidedEndpoint::Stream(update_descriptor),
        });
        staging.provided.push(ProvidedDraft {
            capability_type: snapshot_role.to_owned(),
            capability_instance: snapshot_capability,
            endpoint: ProvidedEndpoint::Rpc(snapshot_descriptor),
        });
        Ok((
            native,
            update_json,
            snapshot_json,
            update_authority.to_string(),
            snapshot_capability.to_string(),
            maximum_transforms,
        ))
    }

    fn require_dynamic_tf_native(
        &self,
        py: Python<'_>,
        requirement_name: &str,
        slot: &str,
        buffer: Py<PyTfTransformBuffer>,
    ) -> PyResult<(Py<PyAny>, String)> {
        let binding = {
            let staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            staging.binding(requirement_name)?
        };
        if binding.provider != binding.descriptor.provider_component {
            return Err(tf_pairing_error(
                py,
                "dynamic TF provider does not match its capability descriptor",
            ));
        }
        let capability_instance = binding.descriptor.capability_instance;
        let descriptor = binding.descriptor.try_stream().cloned().ok_or_else(|| {
            tf_pairing_error(py, "dynamic TF requirement must resolve to a stream")
        })?;
        let expected = Stream::<DynamicTransform>::descriptor(
            capability_instance.stream_service_name(),
            StreamProfile::Control,
        );
        if descriptor != expected {
            return Err(tf_pairing_error(
                py,
                "dynamic TF requirement descriptor does not match the canonical Control stream",
            ));
        }
        let descriptor_json = serde_json::to_string(&descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let buffer_arg = buffer.clone_ref(py).into_any();
        let native = py
            .import("rbt._rbt")?
            .getattr("_DynamicTfSubscriber")?
            .getattr("open")?
            .call1((
                descriptor_json.as_str(),
                capability_instance.to_string(),
                slot,
                buffer_arg,
            ))?
            .unbind();

        let mut staging = lock_staging(&self.staging)?;
        if let Err(error) = staging.ensure_open() {
            drop(staging);
            close_tf_endpoint(&native);
            return Err(error);
        }
        staging
            .endpoints
            .push(StagedEndpoint::DynamicTfSubscriber(native.clone_ref(py)));
        Ok((native, descriptor_json))
    }

    fn require_static_tf_native(
        &self,
        py: Python<'_>,
        update_requirement: &str,
        snapshot_requirement: &str,
        slot: &str,
        buffer: Py<PyTfTransformBuffer>,
        maximum_transforms: Bound<'_, PyAny>,
    ) -> PyResult<(Py<PyAny>, String, String, String, String, usize)> {
        if update_requirement == snapshot_requirement {
            return Err(tf_pairing_error(
                py,
                "static update and snapshot must use distinct requirements",
            ));
        }
        let maximum_transforms = tf_transport::static_bound(&maximum_transforms)?;
        let (update_binding, snapshot_binding, activation) = {
            let staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            (
                staging.binding(update_requirement)?,
                staging.binding(snapshot_requirement)?,
                staging.activation,
            )
        };
        let update_descriptor = update_binding
            .descriptor
            .try_stream()
            .cloned()
            .ok_or_else(|| tf_pairing_error(py, "static TF update requirement must be a stream"))?;
        let snapshot_descriptor = snapshot_binding
            .descriptor
            .try_rpc()
            .cloned()
            .ok_or_else(|| tf_pairing_error(py, "static TF snapshot requirement must be an RPC"))?;

        if update_binding.provider != snapshot_binding.provider
            || update_binding.descriptor.provider_component
                != snapshot_binding.descriptor.provider_component
            || update_binding.descriptor.provider_process
                != snapshot_binding.descriptor.provider_process
            || update_binding.descriptor.activation != snapshot_binding.descriptor.activation
            || update_binding.provider != update_binding.descriptor.provider_component
            || snapshot_binding.provider != snapshot_binding.descriptor.provider_component
            || update_binding.descriptor.capability_instance
                == snapshot_binding.descriptor.capability_instance
        {
            return Err(tf_pairing_error(
                py,
                "static update and snapshot must belong to the same provider process and activation",
            ));
        }

        let update_authority = update_binding.descriptor.capability_instance;
        let snapshot_capability = snapshot_binding.descriptor.capability_instance;
        let expected_update = SliceStream::<StaticUpdate>::descriptor(
            update_authority.stream_service_name(),
            StreamProfile::Control,
        );
        let rpc_resources = RpcResourceConfig {
            max_response_slice_len: maximum_transforms,
            ..RpcResourceConfig::default()
        };
        let expected_snapshot = RpcDescriptor::for_fixed_slice_with_resources::<
            StaticSnapshotRequest,
            StaticSnapshot,
        >(snapshot_capability.rpc_service_name(), rpc_resources);
        if update_descriptor != expected_update || snapshot_descriptor != expected_snapshot {
            return Err(tf_pairing_error(
                py,
                "static TF capability descriptors do not match the requested typed pair and bound",
            ));
        }

        let update_json = serde_json::to_string(&update_descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let snapshot_json = serde_json::to_string(&snapshot_descriptor)
            .map_err(|error| RbtError::new_err(error.to_string()))?;
        let buffer_arg = buffer.clone_ref(py).into_any();
        let native = py
            .import("rbt._rbt")?
            .getattr("_StaticTfClient")?
            .getattr("open")?
            .call1((
                update_authority.to_string(),
                update_json.as_str(),
                snapshot_json.as_str(),
                snapshot_capability.to_string(),
                activation.value(),
                slot,
                maximum_transforms,
                buffer_arg,
            ))?
            .unbind();

        let mut staging = lock_staging(&self.staging)?;
        if let Err(error) = staging.ensure_open() {
            drop(staging);
            close_tf_endpoint(&native);
            return Err(error);
        }
        staging
            .endpoints
            .push(StagedEndpoint::StaticTfClient(native.clone_ref(py)));
        Ok((
            native,
            update_json,
            snapshot_json,
            update_authority.to_string(),
            snapshot_capability.to_string(),
            maximum_transforms,
        ))
    }
}

fn validate_tf_role(role: &str, name: &str) -> PyResult<()> {
    if role.trim().is_empty() {
        Err(PyValueError::new_err(format!(
            "{name} must be non-empty text"
        )))
    } else {
        Ok(())
    }
}

fn tf_pairing_error(py: Python<'_>, message: &'static str) -> pyo3::PyErr {
    tf_transport::transport_py_error(py, TransportError::PairingInvalid(message))
}

#[pymethods]
impl PyActivationContext {
    /// Returns the stable logical component identity.
    #[getter]
    fn component_id(&self) -> PyResult<String> {
        Ok(lock_staging(&self.staging)?.component.to_string())
    }

    /// Alias for [`Self::component_id`].
    #[getter]
    fn component(&self) -> PyResult<String> {
        self.component_id()
    }

    /// Returns the process lifetime UUID as a string.
    #[getter]
    fn process_instance_id(&self) -> PyResult<String> {
        Ok(lock_staging(&self.staging)?.process.to_string())
    }

    /// Alias for [`Self::process_instance_id`].
    #[getter]
    fn process_instance(&self) -> PyResult<String> {
        self.process_instance_id()
    }

    /// Returns the activation number.
    #[getter]
    fn activation_id(&self) -> PyResult<u64> {
        Ok(lock_staging(&self.staging)?.activation.value())
    }

    /// Alias for [`Self::activation_id`].
    #[getter]
    fn activation(&self) -> PyResult<u64> {
        self.activation_id()
    }

    /// Returns whether activation cancellation/expiry has been requested.
    #[getter]
    fn cancelled(&self) -> PyResult<bool> {
        Ok(lock_staging(&self.staging)?.guard.is_expired())
    }

    fn is_cancelled(&self) -> PyResult<bool> {
        self.cancelled()
    }

    /// Resolves a semantic requirement to an activation-bound native handle.
    #[pyo3(signature = (requirement_name, max_borrowed=None))]
    fn require(
        &self,
        py: Python<'_>,
        requirement_name: &str,
        max_borrowed: Option<usize>,
    ) -> PyResult<(String, Py<PyAny>, String, String, String)> {
        self.require_native(py, requirement_name, max_borrowed)
    }

    /// Alias retained for wrappers that keep native calls private.
    #[pyo3(signature = (requirement_name, max_borrowed=None))]
    fn _require(
        &self,
        py: Python<'_>,
        requirement_name: &str,
        max_borrowed: Option<usize>,
    ) -> PyResult<(String, Py<PyAny>, String, String, String)> {
        self.require_native(py, requirement_name, max_borrowed)
    }

    /// Resolves a fixed-size RPC capability to an activation-owned client.
    /// The request and response ABI are taken from the resolver-selected
    /// descriptor; Python does not reopen a service by name.
    #[pyo3(signature = (requirement_name))]
    fn require_rpc(
        &self,
        py: Python<'_>,
        requirement_name: &str,
    ) -> PyResult<(String, Py<PyAny>, String, String, String)> {
        self.require_native(py, requirement_name, None)
    }

    #[pyo3(signature = (capability_type, maximum_payload_bytes=6_220_800, max_loans=2, max_borrowed=1))]
    fn provide_image(
        &self,
        py: Python<'_>,
        capability_type: &str,
        maximum_payload_bytes: usize,
        max_loans: usize,
        max_borrowed: usize,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        self.provide_image_native(
            py,
            capability_type,
            maximum_payload_bytes,
            max_loans,
            max_borrowed,
        )
    }

    #[pyo3(signature = (capability_type, maximum_payload_bytes=6_220_800, max_loans=2, max_borrowed=1))]
    fn _provide_image(
        &self,
        py: Python<'_>,
        capability_type: &str,
        maximum_payload_bytes: usize,
        max_loans: usize,
        max_borrowed: usize,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        self.provide_image_native(
            py,
            capability_type,
            maximum_payload_bytes,
            max_loans,
            max_borrowed,
        )
    }

    #[pyo3(signature = (capability_type, maximum_points=100_000, max_loans=2, max_borrowed=1))]
    fn provide_point_cloud(
        &self,
        py: Python<'_>,
        capability_type: &str,
        maximum_points: usize,
        max_loans: usize,
        max_borrowed: usize,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        self.provide_pointcloud_native(py, capability_type, maximum_points, max_loans, max_borrowed)
    }

    #[pyo3(signature = (capability_type, maximum_points=100_000, max_loans=2, max_borrowed=1))]
    fn provide_pointcloud(
        &self,
        py: Python<'_>,
        capability_type: &str,
        maximum_points: usize,
        max_loans: usize,
        max_borrowed: usize,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        self.provide_pointcloud_native(py, capability_type, maximum_points, max_loans, max_borrowed)
    }

    #[pyo3(signature = (capability_type, maximum_points=100_000, max_loans=2, max_borrowed=1))]
    fn _provide_point_cloud(
        &self,
        py: Python<'_>,
        capability_type: &str,
        maximum_points: usize,
        max_loans: usize,
        max_borrowed: usize,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        self.provide_pointcloud_native(py, capability_type, maximum_points, max_loans, max_borrowed)
    }

    /// Opens and stages a fixed-size RPC server.  The descriptor's physical
    /// service name is replaced with a fresh activation-owned capability
    /// instance before publication.
    fn provide_rpc(
        &self,
        capability_type: &str,
        descriptor_json: &str,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        Python::attach(|py| self.provide_rpc_native(py, capability_type, descriptor_json))
    }

    fn _provide_rpc(
        &self,
        capability_type: &str,
        descriptor_json: &str,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        Python::attach(|py| self.provide_rpc_native(py, capability_type, descriptor_json))
    }

    #[pyo3(signature=(capability_type))]
    fn provide_dynamic_tf(
        &self,
        py: Python<'_>,
        capability_type: &str,
    ) -> PyResult<(Py<PyAny>, String, String)> {
        self.provide_dynamic_tf_native(py, capability_type)
    }

    #[pyo3(signature=(update_role, snapshot_role, initial, maximum_transforms))]
    fn provide_static_tf(
        &self,
        py: Python<'_>,
        update_role: &str,
        snapshot_role: &str,
        initial: Bound<'_, PyAny>,
        maximum_transforms: Bound<'_, PyAny>,
    ) -> PyResult<(Py<PyAny>, String, String, String, String, usize)> {
        self.provide_static_tf_native(py, update_role, snapshot_role, initial, maximum_transforms)
    }

    #[pyo3(signature=(requirement_name, slot, buffer))]
    fn require_dynamic_tf(
        &self,
        py: Python<'_>,
        requirement_name: &str,
        slot: &str,
        buffer: Py<PyTfTransformBuffer>,
    ) -> PyResult<(Py<PyAny>, String)> {
        self.require_dynamic_tf_native(py, requirement_name, slot, buffer)
    }

    #[pyo3(signature=(update_requirement, snapshot_requirement, slot, buffer, maximum_transforms))]
    fn require_static_tf(
        &self,
        py: Python<'_>,
        update_requirement: &str,
        snapshot_requirement: &str,
        slot: &str,
        buffer: Py<PyTfTransformBuffer>,
        maximum_transforms: Bound<'_, PyAny>,
    ) -> PyResult<(Py<PyAny>, String, String, String, String, usize)> {
        self.require_static_tf_native(
            py,
            update_requirement,
            snapshot_requirement,
            slot,
            buffer,
            maximum_transforms,
        )
    }

    fn spawn_critical(&self, name: &str, callable: Py<PyAny>) -> PyResult<()> {
        self.stage_task(name, callable, true)
    }

    fn spawn_background(&self, name: &str, callable: Py<PyAny>) -> PyResult<()> {
        self.stage_task(name, callable, false)
    }

    fn spawn_critical_async(
        &self,
        py: Python<'_>,
        name: &str,
        callable: Py<PyAny>,
    ) -> PyResult<Py<PyAny>> {
        self.stage_async_task(py, name, callable, true)
    }

    fn spawn_background_async(
        &self,
        py: Python<'_>,
        name: &str,
        callable: Py<PyAny>,
    ) -> PyResult<Py<PyAny>> {
        self.stage_async_task(py, name, callable, false)
    }

    fn _spawn_critical(&self, name: &str, callable: Py<PyAny>) -> PyResult<()> {
        self.stage_task(name, callable, true)
    }

    fn _spawn_background(&self, name: &str, callable: Py<PyAny>) -> PyResult<()> {
        self.stage_task(name, callable, false)
    }

    fn _spawn_critical_async(
        &self,
        py: Python<'_>,
        name: &str,
        callable: Py<PyAny>,
    ) -> PyResult<Py<PyAny>> {
        self.stage_async_task(py, name, callable, true)
    }

    fn _spawn_background_async(
        &self,
        py: Python<'_>,
        name: &str,
        callable: Py<PyAny>,
    ) -> PyResult<Py<PyAny>> {
        self.stage_async_task(py, name, callable, false)
    }
}

impl PyActivationContext {
    fn stage_task(&self, name: &str, callable: Py<PyAny>, critical: bool) -> PyResult<()> {
        if name.is_empty() {
            return Err(PyValueError::new_err("task name must not be empty"));
        }
        Python::attach(|py| {
            if !callable.bind(py).is_callable() {
                return Err(PyValueError::new_err("task callable must be callable"));
            }
            let mut staging = lock_staging(&self.staging)?;
            staging.ensure_open()?;
            staging.tasks.push(StagedTask {
                name: name.to_owned(),
                callable,
                critical,
                asynchronous: false,
                async_control: None,
            });
            Ok(())
        })
    }

    fn stage_async_task(
        &self,
        py: Python<'_>,
        name: &str,
        callable: Py<PyAny>,
        critical: bool,
    ) -> PyResult<Py<PyAny>> {
        if name.is_empty() {
            return Err(PyValueError::new_err("task name must not be empty"));
        }
        if !callable.bind(py).is_callable() {
            return Err(PyValueError::new_err("task callable must be callable"));
        }
        let control = AsyncTaskControl::new(&self.async_runtime);
        let handle = Py::new(
            py,
            PyAsyncTaskHandle {
                control: Arc::clone(&control),
            },
        )?;
        let mut staging = lock_staging(&self.staging)?;
        staging.ensure_open()?;
        staging.tasks.push(StagedTask {
            name: name.to_owned(),
            callable,
            critical,
            asynchronous: true,
            async_control: Some(control),
        });
        Ok(handle.into_any())
    }
}

/// Small task context passed to each Python worker callback.
#[pyclass(module = "rbt._rbt", name = "_TaskContext")]
pub(crate) struct PyTaskContext {
    token: CancellationToken,
}

#[pymethods]
impl PyTaskContext {
    #[getter]
    fn cancelled(&self) -> bool {
        self.token.is_cancelled()
    }

    fn is_cancelled(&self) -> bool {
        self.token.is_cancelled()
    }

    fn throw_if_cancelled(&self) -> PyResult<()> {
        if self.token.is_cancelled() {
            Err(ActivationExpiredError::new_err(
                "task cancellation has been requested",
            ))
        } else {
            Ok(())
        }
    }
}

/// A handle for one activation-owned Python asyncio task.
#[pyclass(module = "rbt._rbt", name = "_AsyncTaskHandle")]
pub(crate) struct PyAsyncTaskHandle {
    control: Arc<AsyncTaskControl>,
}

#[pymethods]
impl PyAsyncTaskHandle {
    /// Requests cancellation of this task only.  The activation supervisor
    /// observes a critical task's resulting `CancelledError` as an
    /// unexpected critical exit; activation teardown marks cancellation
    /// separately and is treated as normal.
    fn cancel(&self) {
        self.control.cancel();
    }

    #[getter]
    fn cancelled(&self) -> bool {
        self.control.is_cancelled()
    }

    fn is_cancelled(&self) -> bool {
        self.control.is_cancelled()
    }

    #[getter]
    fn done(&self) -> bool {
        self.control.is_done()
    }

    fn is_done(&self) -> bool {
        self.control.is_done()
    }
}

fn run_python_task(callable: Py<PyAny>, token: CancellationToken) -> Result<(), PythonTaskError> {
    Python::attach(|py| {
        let task_context =
            Py::new(py, PyTaskContext { token }).map_err(|error| PythonTaskError {
                diagnostic: py_diagnostic(py, "task", error),
            })?;
        callable
            .call1(py, (task_context.bind(py),))
            .map(|_| ())
            .map_err(|error| PythonTaskError {
                diagnostic: py_diagnostic(py, "task", error),
            })
    })
}

fn async_task_outcome_result(
    outcome: Result<AsyncTaskOutcome, oneshot::error::RecvError>,
) -> Result<(), PythonTaskError> {
    match outcome {
        Ok(AsyncTaskOutcome::Completed | AsyncTaskOutcome::Cancelled) => Ok(()),
        Ok(AsyncTaskOutcome::Error(diagnostic)) => Err(async_task_error(diagnostic)),
        Err(_) => Err(PythonTaskError {
            diagnostic: PythonDiagnostic {
                phase: "async task",
                class: "AsyncRuntimeClosedError".to_owned(),
                message: "managed asyncio task completion was disconnected".to_owned(),
                traceback: String::new(),
            },
        }),
    }
}

fn register_staged_endpoint(scope: &mut rbt_compose::EffectScope, endpoint: StagedEndpoint) {
    match endpoint {
        StagedEndpoint::ImagePublisher(cell) => {
            scope.defer_sync(move || close_image_publisher(&cell));
        }
        StagedEndpoint::PointCloudPublisher(cell) => {
            scope.defer_sync(move || close_pointcloud_publisher(&cell));
        }
        StagedEndpoint::ImageSubscriber(cell) => {
            scope.defer_sync(move || close_image_subscriber(&cell));
        }
        StagedEndpoint::PointCloudSubscriber(cell) => {
            scope.defer_sync(move || close_pointcloud_subscriber(&cell));
        }
        StagedEndpoint::RpcClient(cell) => {
            scope.defer_sync(move || close_client(&cell));
        }
        StagedEndpoint::RpcServer(cell) => {
            scope.defer_sync(move || close_server(&cell));
        }
        StagedEndpoint::DynamicTfPublisher(endpoint)
        | StagedEndpoint::DynamicTfSubscriber(endpoint)
        | StagedEndpoint::StaticTfProvider(endpoint)
        | StagedEndpoint::StaticTfClient(endpoint) => {
            let shutdown_endpoint = Python::attach(|py| endpoint.clone_ref(py));
            scope.wake_on_shutdown(move || close_tf_endpoint(&shutdown_endpoint));
            scope.defer_sync(move || close_tf_endpoint(&endpoint));
        }
    }
}

fn commit_activation(
    ctx: &mut ActivationCtx<'_>,
    staging: Arc<Mutex<ActivationStaging>>,
) -> Result<(), PythonLifecycleError> {
    let (guard, async_runtime, endpoints, provided, tasks) = {
        let mut staging = staging.lock().map_err(|_| PythonLifecycleError {
            diagnostic: PythonDiagnostic {
                phase: "activate",
                class: "RustPoisonError".to_owned(),
                message: "activation staging lock was poisoned".to_owned(),
                traceback: String::new(),
            },
        })?;
        if !staging.sealed {
            return Err(PythonLifecycleError {
                diagnostic: PythonDiagnostic {
                    phase: "activate",
                    class: "RustActivationError".to_owned(),
                    message: "activation staging was not sealed".to_owned(),
                    traceback: String::new(),
                },
            });
        }
        (
            Arc::clone(&staging.guard),
            Arc::clone(&staging.async_runtime),
            std::mem::take(&mut staging.endpoints),
            std::mem::take(&mut staging.provided),
            std::mem::take(&mut staging.tasks),
        )
    };

    // Endpoint closers intentionally precede task actions. EffectScope's
    // LIFO shutdown therefore joins every supervised Python worker before it
    // closes a subscriber/publisher endpoint.
    for endpoint in endpoints {
        register_staged_endpoint(ctx.scope(), endpoint);
    }
    for capability in provided {
        match capability.endpoint {
            ProvidedEndpoint::Stream(descriptor) => ctx.provide_untyped(
                capability.capability_type,
                capability.capability_instance,
                descriptor,
            ),
            ProvidedEndpoint::Rpc(descriptor) => ctx.provide_rpc_untyped(
                capability.capability_type,
                capability.capability_instance,
                descriptor,
            ),
        }
    }
    if tasks.iter().any(|task| task.asynchronous) {
        guard.start_async_bridge();
    }
    for task in tasks {
        let task_name = task.name;
        let callable = task.callable;
        if task.asynchronous {
            let control = task
                .async_control
                .expect("staged async task must carry its cancellation control");
            let runtime = Arc::clone(&async_runtime);
            let task_runner = move |token: CancellationToken| {
                let callable = callable;
                let control = Arc::clone(&control);
                let runtime = Arc::clone(&runtime);
                async move {
                    if token.is_cancelled() {
                        control.cancel_for_activation();
                        control.mark_done();
                        return Ok(());
                    }
                    let task_context = Python::attach(|py| {
                        Py::new(
                            py,
                            PyTaskContext {
                                token: token.clone(),
                            },
                        )
                        .map(|context| context.into_any())
                        .map_err(|error| PythonTaskError {
                            diagnostic: py_diagnostic(py, "async task", error),
                        })
                    })?;
                    let (completion_tx, completion_rx) = oneshot::channel();
                    if let Err(error) = runtime.spawn_coroutine(
                        callable,
                        task_context,
                        Arc::clone(&control),
                        completion_tx,
                    ) {
                        control.mark_done();
                        return Err(PythonTaskError {
                            diagnostic: PythonDiagnostic {
                                phase: "async task",
                                class: "AsyncRuntimeClosedError".to_owned(),
                                message: error.to_string(),
                                traceback: String::new(),
                            },
                        });
                    }
                    tokio::pin!(completion_rx);
                    tokio::select! {
                        outcome = &mut completion_rx => async_task_outcome_result(outcome),
                        _ = token.cancelled() => {
                            control.cancel_for_activation();
                            let _ = completion_rx.await;
                            Ok(())
                        }
                    }
                }
            };
            if task.critical {
                ctx.spawn_critical(task_name, task_runner);
            } else {
                ctx.spawn_background(task_name, task_runner);
            }
        } else {
            let guard_for_wake = Arc::clone(&guard);
            let wake = move || guard_for_wake.expire();
            let task_runner = move |token: CancellationToken| run_python_task(callable, token);
            if task.critical {
                ctx.spawn_critical_blocking(task_name, task_runner, wake);
            } else {
                ctx.spawn_background_blocking(task_name, task_runner, wake);
            }
        }
    }
    // Register expiry last so LIFO shutdown marks the activation stale before
    // joining tasks or closing endpoints. Blocking-task wake callbacks also
    // call this same idempotent path.
    let guard_for_cleanup = Arc::clone(&guard);
    ctx.scope().defer_sync(move || guard_for_cleanup.expire());
    Ok(())
}

struct PythonComponentConfig {
    adapter: Py<PyAny>,
    config: Py<PyAny>,
    component: ComponentId,
    requirements: Vec<RequirementSpec>,
    async_runtime: Arc<AsyncioRuntime>,
}

struct PythonComponent {
    component: Py<PyAny>,
    component_id: ComponentId,
    requirements: Vec<RequirementSpec>,
    async_runtime: Arc<AsyncioRuntime>,
}

#[async_trait]
impl Component for PythonComponent {
    type Config = PythonComponentConfig;
    type Error = PythonLifecycleError;

    async fn prepare(config: &Self::Config) -> Result<Self, Self::Error> {
        Python::attach(|py| {
            config
                .adapter
                .call_method1(py, "prepare", (config.config.bind(py),))
                .map_err(|error| PythonLifecycleError {
                    diagnostic: py_diagnostic(py, "prepare", error),
                })
        })?;
        let component = Python::attach(|py| config.adapter.clone_ref(py));
        Ok(Self {
            component,
            component_id: config.component.clone(),
            requirements: config.requirements.clone(),
            async_runtime: Arc::clone(&config.async_runtime),
        })
    }

    fn id(&self) -> ComponentId {
        self.component_id.clone()
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        self.requirements.clone()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<(), Self::Error> {
        let guard = ActivationGuard::new(ctx.process_instance(), ctx.activation_id());
        let bindings = self
            .requirements
            .iter()
            .map(|requirement| {
                ctx.binding_untyped(&requirement.requirement_name, &requirement.capability_type)
                    .map_err(|error| PythonLifecycleError {
                        diagnostic: PythonDiagnostic {
                            phase: "activate",
                            class: "RequirementError".to_owned(),
                            message: error.to_string(),
                            traceback: String::new(),
                        },
                    })
            })
            .collect::<Result<Vec<_>, _>>()?;
        let staging = Arc::new(Mutex::new(ActivationStaging::new(
            ctx.component().clone(),
            ctx.process_instance(),
            ctx.activation_id(),
            Arc::clone(&guard),
            Arc::clone(&self.async_runtime),
            bindings,
            self.requirements.clone(),
        )));
        let context = Python::attach(|py| {
            Py::new(
                py,
                PyActivationContext {
                    staging: Arc::clone(&staging),
                    async_runtime: Arc::clone(&self.async_runtime),
                },
            )
        })
        .map_err(|error| PythonLifecycleError {
            diagnostic: Python::attach(|py| py_diagnostic(py, "activate", error)),
        })?;
        let hook_result = Python::attach(|py| {
            self.component
                .call_method1(py, "activate", (context.bind(py),))
                .map(|_| ())
                .map_err(|error| PythonLifecycleError {
                    diagnostic: py_diagnostic(py, "activate", error),
                })
        });
        match hook_result {
            Ok(()) => {
                if let Ok(mut staging_state) = staging.lock() {
                    staging_state.sealed = true;
                }
                commit_activation(ctx, staging)
            }
            Err(error) => {
                if let Ok(mut staging_state) = staging.lock() {
                    staging_state.close();
                }
                Err(error)
            }
        }
    }
}

fn extract_capability_type(value: Bound<'_, PyAny>) -> PyResult<String> {
    if let Ok(value) = value.extract::<String>() {
        return Ok(value);
    }
    value
        .getattr("TYPE_NAME")
        .and_then(|value| value.extract::<String>())
        .map_err(|_| PyValueError::new_err("capability type must be a string or TYPE_NAME"))
}

fn parse_requirements(value: Bound<'_, PyAny>) -> PyResult<Vec<RequirementSpec>> {
    let mut requirements = Vec::new();
    if let Ok(mapping) = value.cast::<PyDict>() {
        for (name, capability) in mapping.iter() {
            let name = name.extract::<String>()?;
            let capability = extract_capability_type(capability)?;
            requirements.push(RequirementSpec::new(name, capability));
        }
    } else {
        for item in value.try_iter()? {
            let item = item?;
            let pair = item.extract::<(String, Bound<'_, PyAny>)>()?;
            requirements.push(RequirementSpec::new(
                pair.0,
                extract_capability_type(pair.1)?,
            ));
        }
    }
    let mut names = HashSet::new();
    for requirement in &requirements {
        if requirement.requirement_name.is_empty() {
            return Err(PyValueError::new_err("requirement name must not be empty"));
        }
        if requirement.capability_type.is_empty() {
            return Err(PyValueError::new_err("capability type must not be empty"));
        }
        if !names.insert(requirement.requirement_name.clone()) {
            return Err(PyValueError::new_err(format!(
                "duplicate requirement name {:?}",
                requirement.requirement_name
            )));
        }
    }
    Ok(requirements)
}

fn install_tracing() {
    static INIT: std::sync::Once = std::sync::Once::new();
    INIT.call_once(|| {
        let filter = tracing_subscriber::EnvFilter::try_from_default_env()
            .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info"));
        // Component logs are usually redirected to files or supervisors; only
        // emit ANSI styling when stdout (the fmt default writer) is a terminal.
        let _ = tracing_subscriber::fmt()
            .with_env_filter(filter)
            .with_target(false)
            .with_ansi(std::io::IsTerminal::is_terminal(&std::io::stdout()))
            .try_init();
    });
}

#[pyfunction]
pub(crate) fn _run_component(
    py: Python<'_>,
    component_adapter: Bound<'_, PyAny>,
    component_id: &str,
    requirements: Bound<'_, PyAny>,
    config: Bound<'_, PyAny>,
    composer_address: &str,
) -> PyResult<()> {
    install_tracing();
    let requirements = parse_requirements(requirements)?;
    let config = config.unbind();
    let adapter = component_adapter.unbind();
    let async_runtime = AsyncioRuntime::new();
    let config = PythonComponentConfig {
        adapter,
        config,
        component: ComponentId::new(component_id),
        requirements,
        async_runtime: Arc::clone(&async_runtime),
    };
    let composer_address = composer_address.to_owned();
    let result = py.detach(move || {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let runtime = tokio::runtime::Builder::new_multi_thread()
                .enable_all()
                .build()
                .map_err(|error| error.to_string())?;
            runtime
                .block_on(rbt_compose::run_component::<PythonComponent, _>(
                    config,
                    composer_address,
                ))
                .map_err(|error| error.to_string())
        }));
        async_runtime.shutdown();
        match result {
            Ok(result) => result,
            Err(_) => Err("Python component runtime panicked".to_owned()),
        }
    });
    match result {
        Ok(()) => Ok(()),
        Err(error) if error.contains("component failed before connecting") => {
            Err(ComponentPreparationError::new_err(error))
        }
        Err(error) => Err(ComponentRuntimeError::new_err(error)),
    }
}

pub(crate) fn add_to_module(py: Python<'_>, module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyActivationContext>()?;
    module.add_class::<PyTaskContext>()?;
    module.add_class::<PyAsyncTaskHandle>()?;
    module.add_function(wrap_pyfunction!(_run_component, module)?)?;
    module.add(
        "ComponentRuntimeError",
        py.get_type::<ComponentRuntimeError>(),
    )?;
    module.add(
        "ComponentPreparationError",
        py.get_type::<ComponentPreparationError>(),
    )?;
    module.add(
        "ComponentActivationError",
        py.get_type::<ComponentActivationError>(),
    )?;
    module.add("TaskError", py.get_type::<super::TaskError>())?;
    module.add(
        "ActivationExpiredError",
        py.get_type::<ActivationExpiredError>(),
    )?;
    module.add(
        "ConcurrentReceiveError",
        py.get_type::<super::ConcurrentReceiveError>(),
    )?;
    module.add(
        "AsyncRuntimeClosedError",
        py.get_type::<super::AsyncRuntimeClosedError>(),
    )?;
    Ok(())
}

//! CPython data binding for rbt shared-memory streams.
//!
//! Received samples expose read-only buffers. Publisher loans expose writable
//! buffers through an explicit native lease whose lifetime gates consuming the
//! underlying shared-memory sample.

#![forbid(unsafe_op_in_unsafe_fn)]

use std::{
    ffi::{CString, c_int, c_void},
    mem::{align_of, size_of, size_of_val},
    ptr,
    sync::{Arc, Mutex, MutexGuard},
    time::Duration,
};

use pyo3::{
    Bound, PyErr, PyResult, Python, create_exception,
    exceptions::{PyBufferError, PyException, PyValueError},
    ffi,
    prelude::*,
    types::{PyAny, PyDict, PyModule},
};
use rbt_core::{ActivationId, ProcessInstanceId};
use rbt_iox2::{
    Image, ImageHeader, PointCloud, ReceivedSliceSample, SliceStream, StreamError, StreamPressure,
    StreamResourceConfig, WaitContext, WaitError as NativeWaitError, WaitProgress,
    WaitStatsSnapshot, WaitToken,
};
use rbt_time_stream::{StampedBytes, TIMING_PREFIX_LEN, decode_timing};
use thiserror::Error;

mod async_runtime;
mod component_runtime;
mod rpc;
mod tf;
mod tf_transport;
use async_runtime::{AsyncActivationBridge, AsyncSubscriberState};
use component_runtime::ActivationGuard;

create_exception!(rbt, RbtError, PyException);
create_exception!(rbt, AbiMismatchError, RbtError);
create_exception!(rbt, BorrowLimitReachedError, RbtError);
create_exception!(rbt, InvalidResourceConfigError, RbtError);
create_exception!(rbt, IncompatibleResourceConfigError, RbtError);
create_exception!(rbt, WaitError, RbtError);
create_exception!(rbt, InvalidPayloadError, RbtError);
create_exception!(rbt, InvalidImageLayoutError, InvalidPayloadError);
create_exception!(rbt, InvalidPayloadAlignmentError, InvalidPayloadError);
create_exception!(rbt, UnsupportedPixelFormatError, RbtError);
create_exception!(rbt, MissingTransportFeatureError, RbtError);
create_exception!(rbt, LoanPoolExhaustedError, RbtError);
create_exception!(rbt, LoanStillMappedError, RbtError);
create_exception!(rbt, LoanConsumedError, RbtError);
create_exception!(rbt, QueueFullError, RbtError);
create_exception!(rbt, UnableToDeliverError, RbtError);
create_exception!(rbt, ReadinessNotifyError, RbtError);
create_exception!(rbt, ComponentRuntimeError, RbtError);
create_exception!(rbt, ComponentPreparationError, ComponentRuntimeError);
create_exception!(rbt, ComponentActivationError, ComponentRuntimeError);
create_exception!(rbt, TaskError, ComponentRuntimeError);
create_exception!(rbt, ActivationExpiredError, ComponentRuntimeError);
create_exception!(rbt, ConcurrentReceiveError, RbtError);
create_exception!(rbt, AsyncRuntimeClosedError, RbtError);
create_exception!(rbt, RpcError, RbtError);
create_exception!(rbt, RpcUnavailableError, RpcError);
create_exception!(rbt, RpcDeadlineExceededError, RpcError);
create_exception!(rbt, RpcCancelledError, RpcError);
create_exception!(rbt, RpcCapacityError, RpcError);
create_exception!(rbt, RpcActivationExpiredError, ActivationExpiredError);
create_exception!(rbt, RpcTransportError, RpcError);

#[derive(Debug, Error)]
enum ReceiveError {
    #[error(transparent)]
    Stream(#[from] StreamError),
    #[error(transparent)]
    Wait(#[from] NativeWaitError),
}

fn stream_py_error(error: StreamError) -> PyErr {
    stream_py_error_with_borrow_limit(error, rbt_iox2::MAX_BORROWED_SAMPLES)
}

fn stream_py_error_with_borrow_limit(error: StreamError, max_borrowed: usize) -> PyErr {
    let message = error.to_string();
    match error {
        StreamError::AbiMismatch { .. } => AbiMismatchError::new_err(message),
        StreamError::BorrowLimitReached(_) => {
            BorrowLimitReachedError::new_err(format!("{message}; max borrowed={max_borrowed}"))
        }
        StreamError::InvalidResourceConfig { .. } => InvalidResourceConfigError::new_err(message),
        StreamError::IncompatibleResourceConfig { .. } => {
            IncompatibleResourceConfigError::new_err(message)
        }
        StreamError::MissingTransportFeature { .. } => {
            MissingTransportFeatureError::new_err(message)
        }
        StreamError::InvalidPayloadLength { .. } => InvalidPayloadError::new_err(message),
        StreamError::LoanPoolExhausted(_) => LoanPoolExhaustedError::new_err(message),
        StreamError::QueueFull => QueueFullError::new_err(message),
        StreamError::UnableToDeliver => UnableToDeliverError::new_err(message),
        StreamError::ReadinessNotify(_) => ReadinessNotifyError::new_err(message),
        _ => RbtError::new_err(message),
    }
}

fn receive_py_error(error: ReceiveError) -> PyErr {
    match error {
        ReceiveError::Stream(error) => stream_py_error(error),
        ReceiveError::Wait(error) => WaitError::new_err(error.to_string()),
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct CumulativeWaitStats {
    waits: u64,
    wakeups: u64,
    empty_wakeups: u64,
    cancellation_wakeups: u64,
    samples_processed: u64,
}

impl CumulativeWaitStats {
    fn add(&mut self, snapshot: WaitStatsSnapshot) {
        self.waits = self.waits.saturating_add(snapshot.waits);
        self.wakeups = self.wakeups.saturating_add(snapshot.wakeups);
        self.empty_wakeups = self.empty_wakeups.saturating_add(snapshot.empty_wakeups);
        self.cancellation_wakeups = self
            .cancellation_wakeups
            .saturating_add(snapshot.cancellation_wakeups);
        self.samples_processed = self
            .samples_processed
            .saturating_add(snapshot.samples_processed);
    }
}

struct WaitOutcome<T> {
    sample: Option<T>,
    stats: WaitStatsSnapshot,
}

/// Runs one bounded, event-driven WaitSet receive. The native WaitSet timeout
/// bounds the call directly; no helper thread or payload polling is involved.
fn wait_for_slice<M>(
    subscriber: &rbt_iox2::SliceSubscriber<M>,
    process_instance: ProcessInstanceId,
    activation: ActivationId,
    timeout: Duration,
    guard: Option<&Arc<ActivationGuard>>,
) -> Result<WaitOutcome<ReceivedSliceSample<M>>, ReceiveError>
where
    M: rbt_types::SliceMessageAbi,
    M::Header: Default + iceoryx2::prelude::ZeroCopySend,
    M::Element: Default + iceoryx2::prelude::ZeroCopySend,
{
    let mut context = WaitContext::new(process_instance, activation)?;
    let source = subscriber.event_source(WaitToken::new(1));
    let wake_handle = context.wake_handle();
    let _activation_registration = guard.map(|guard| guard.register_wait(wake_handle.clone()));
    let mut received = None;

    let _: Option<_> =
        context.run_with_timeout(&[source], timeout, |_, _| match subscriber.receive()? {
            Some(sample) => {
                received = Some(sample);
                Ok::<_, ReceiveError>(WaitProgress::StopAfterSamples(1))
            }
            None => Ok::<_, ReceiveError>(WaitProgress::Empty),
        })?;

    Ok(WaitOutcome {
        sample: received,
        stats: context.stats().snapshot(),
    })
}

fn parse_descriptor(json: &str) -> PyResult<rbt_types::StreamDescriptor> {
    serde_json::from_str(json)
        .map_err(|error| PyValueError::new_err(format!("invalid StreamDescriptor JSON: {error}")))
}

fn pressure_object<'py>(py: Python<'py>, pressure: StreamPressure) -> PyResult<Bound<'py, PyAny>> {
    let resources = PyDict::new(py);
    resources.set_item("max_loans", pressure.resources.max_loans)?;
    resources.set_item("max_borrowed", pressure.resources.max_borrowed)?;

    let publisher = if let Some(value) = pressure.publisher {
        let result = PyDict::new(py);
        result.set_item("loan_attempts", value.loan_attempts)?;
        result.set_item("loan_successes", value.loan_successes)?;
        result.set_item("loans_in_use", value.loans_in_use)?;
        result.set_item("loans_high_water", value.loans_high_water)?;
        result.set_item("loan_failures", value.loan_failures)?;
        result.set_item("payload_bytes_last", value.payload_bytes_last)?;
        result.set_item("payload_bytes_max_seen", value.payload_bytes_max_seen)?;
        result.set_item("total_loaned", value.total_loaned)?;
        result.set_item("send_attempts", value.send_attempts)?;
        result.set_item("send_successes", value.send_successes)?;
        result.set_item("send_failures", value.send_failures)?;
        result.set_item("queue_full", value.queue_full)?;
        result.set_item("unable_to_deliver", value.unable_to_deliver)?;
        result.set_item("readiness_notify_failures", value.readiness_notify_failures)?;
        result.set_item("subscriber_count", value.subscriber_count)?;
        result.set_item("estimated_dropped_samples", value.estimated_dropped_samples)?;
        Some(result.into_any())
    } else {
        None
    };

    let subscriber = if let Some(value) = pressure.subscriber {
        let result = PyDict::new(py);
        result.set_item("receive_attempts", value.receive_attempts)?;
        result.set_item("received_samples", value.received_samples)?;
        result.set_item("borrowed_samples", value.borrowed_samples)?;
        result.set_item("borrowed_high_water", value.borrowed_high_water)?;
        result.set_item("borrow_failures", value.borrow_failures)?;
        result.set_item("borrow_duration_count", value.borrow_duration_count)?;
        result.set_item("borrow_duration_total_ns", value.borrow_duration_total_ns)?;
        result.set_item("borrow_duration_max_ns", value.borrow_duration_max_ns)?;
        result.set_item("oldest_borrow_age_ns", value.oldest_borrow_age_ns)?;
        result.set_item("estimated_dropped_samples", value.estimated_dropped_samples)?;
        Some(result.into_any())
    } else {
        None
    };

    let result = PyDict::new(py);
    result.set_item("service_name", pressure.service_name)?;
    result.set_item("resources", resources)?;
    result.set_item("publisher", publisher)?;
    result.set_item("subscriber", subscriber)?;
    result.set_item(
        "snapshot_timestamp_unix_ms",
        pressure.snapshot_timestamp_unix_ms,
    )?;
    Ok(result.into_any())
}

fn lock_sample<T>(sample: &Mutex<T>) -> PyResult<MutexGuard<'_, T>> {
    sample
        .lock()
        .map_err(|_| RbtError::new_err("received-sample ownership lock was poisoned"))
}

fn endpoint_pressure<'py, E, F>(
    py: Python<'py>,
    endpoint: &Arc<Mutex<Option<E>>>,
    endpoint_name: &str,
    pressure: F,
) -> PyResult<Bound<'py, PyAny>>
where
    F: FnOnce(&E) -> StreamPressure,
{
    let endpoint = endpoint
        .lock()
        .map_err(|_| RbtError::new_err(format!("{endpoint_name} endpoint lock was poisoned")))?;
    let endpoint = endpoint.as_ref().ok_or_else(|| {
        ActivationExpiredError::new_err(format!("{endpoint_name} endpoint is closed"))
    })?;
    pressure_object(py, pressure(endpoint))
}

/// Fills a CPython `Py_buffer` whose owner is the received-sample object.
///
/// # Safety
///
/// `view` must be null or point to writable CPython-owned `Py_buffer` storage.
/// `data` must remain valid while `owner` is alive. Every caller passes a
/// pointer into the `ReceivedSliceSample` stored by that exact owner object.
unsafe fn fill_readonly_buffer(
    view: *mut ffi::Py_buffer,
    flags: c_int,
    data: *const u8,
    len: usize,
    owner: Bound<'_, PyAny>,
) -> PyResult<()> {
    if view.is_null() {
        return Err(PyBufferError::new_err("buffer view is null"));
    }
    if flags & ffi::PyBUF_WRITABLE == ffi::PyBUF_WRITABLE {
        return Err(PyBufferError::new_err(
            "received shared-memory samples are read-only",
        ));
    }
    let len = isize::try_from(len)
        .map_err(|_| PyBufferError::new_err("payload length exceeds Py_ssize_t"))?;

    // SAFETY: `view` was validated non-null above and is exclusively owned by
    // CPython for this getbuffer call. `owner.into_ptr()` transfers one strong
    // reference into `view.obj`; CPython releases it with the buffer. The SHM
    // pointer is immutable and remains valid because owner contains its guard.
    unsafe {
        (*view).obj = owner.into_ptr();
        (*view).buf = data.cast_mut().cast::<c_void>();
        (*view).len = len;
        (*view).readonly = 1;
        (*view).itemsize = 1;
        (*view).format = if flags & ffi::PyBUF_FORMAT == ffi::PyBUF_FORMAT {
            CString::new("B").expect("static buffer format").into_raw()
        } else {
            ptr::null_mut()
        };
        (*view).ndim = 1;
        (*view).shape = if flags & ffi::PyBUF_ND == ffi::PyBUF_ND {
            &mut (*view).len
        } else {
            ptr::null_mut()
        };
        (*view).strides = if flags & ffi::PyBUF_STRIDES == ffi::PyBUF_STRIDES {
            &mut (*view).itemsize
        } else {
            ptr::null_mut()
        };
        (*view).suboffsets = ptr::null_mut();
        (*view).internal = ptr::null_mut();
    }
    Ok(())
}

/// Releases only the optional format string allocated by
/// [`fill_readonly_buffer`]. CPython owns and decrements `view.obj`.
unsafe fn release_readonly_buffer(view: *mut ffi::Py_buffer) {
    if view.is_null() {
        return;
    }
    // SAFETY: a non-null format pointer is created exactly once with
    // `CString::into_raw` above, and CPython invokes releasebuffer once.
    unsafe {
        if !(*view).format.is_null() {
            drop(CString::from_raw((*view).format));
            (*view).format = ptr::null_mut();
        }
    }
}

/// Fills a writable CPython buffer view from a cached shared-memory address.
///
/// The lease object is transferred into `view.obj`, so every CPython buffer
/// view retains the lease and therefore the shared state which owns the
/// underlying initialized loan.  The transient get/release calls themselves
/// do not affect the mapping count; one count is held by each lease root.
unsafe fn fill_writable_buffer(
    view: *mut ffi::Py_buffer,
    flags: c_int,
    address: usize,
    len: usize,
    owner: Bound<'_, PyAny>,
) -> PyResult<()> {
    if view.is_null() {
        return Err(PyBufferError::new_err("buffer view is null"));
    }
    let len = isize::try_from(len)
        .map_err(|_| PyBufferError::new_err("payload length exceeds Py_ssize_t"))?;

    // SAFETY: `view` is non-null and exclusively owned by CPython for this
    // getbuffer call. `owner.into_ptr()` transfers a strong reference to the
    // exact lease that cached `address`; that lease keeps the Active state
    // (and its initialized loan) alive for the entire buffer lifetime.
    unsafe {
        (*view).obj = owner.into_ptr();
        (*view).buf = address as *mut c_void;
        (*view).len = len;
        (*view).readonly = 0;
        (*view).itemsize = 1;
        (*view).format = if flags & ffi::PyBUF_FORMAT == ffi::PyBUF_FORMAT {
            c"B".as_ptr().cast_mut()
        } else {
            ptr::null_mut()
        };
        (*view).ndim = 1;
        (*view).shape = if flags & ffi::PyBUF_ND == ffi::PyBUF_ND {
            &mut (*view).len
        } else {
            ptr::null_mut()
        };
        (*view).strides = if flags & ffi::PyBUF_STRIDES == ffi::PyBUF_STRIDES {
            &mut (*view).itemsize
        } else {
            ptr::null_mut()
        };
        (*view).suboffsets = ptr::null_mut();
        (*view).internal = ptr::null_mut();
    }
    Ok(())
}

#[derive(Clone, Copy, Debug)]
struct ImageLoanMetadata {
    sequence: u64,
    timestamp_ns: u64,
    width: u32,
    height: u32,
    stride: u32,
    encoding: u32,
}

#[derive(Clone, Copy, Debug)]
struct PointCloudLoanMetadata {
    sequence: u64,
    timestamp_ns: u64,
    width: u32,
    height: u32,
    dense: bool,
    point_count: usize,
}

#[derive(Clone, Copy, Debug)]
enum LoanMetadata {
    Image(ImageLoanMetadata),
    PointCloud(PointCloudLoanMetadata),
    StampedBytes { sequence: u64 },
}

type ImagePublisherCell = Arc<Mutex<Option<rbt_iox2::SlicePublisher<Image>>>>;
type PointCloudPublisherCell = Arc<Mutex<Option<rbt_iox2::SlicePublisher<PointCloud>>>>;
type ImageSubscriberCell = Arc<Mutex<Option<rbt_iox2::SliceSubscriber<Image>>>>;
type PointCloudSubscriberCell = Arc<Mutex<Option<rbt_iox2::SliceSubscriber<PointCloud>>>>;
type StampedBytesPublisherCell = Arc<Mutex<Option<rbt_iox2::SlicePublisher<StampedBytes>>>>;
type StampedBytesSubscriberCell = Arc<Mutex<Option<rbt_iox2::SliceSubscriber<StampedBytes>>>>;

fn ensure_activation_active(guard: Option<&Arc<ActivationGuard>>) -> PyResult<()> {
    if let Some(guard) = guard
        && guard.is_expired()
    {
        return Err(ActivationExpiredError::new_err(format!(
            "activation {} for process {} has expired",
            guard.activation(),
            guard.process_instance(),
        )));
    }
    Ok(())
}

fn close_image_publisher(cell: &ImagePublisherCell) {
    if let Ok(mut endpoint) = cell.lock() {
        endpoint.take();
    }
}

fn close_pointcloud_publisher(cell: &PointCloudPublisherCell) {
    if let Ok(mut endpoint) = cell.lock() {
        endpoint.take();
    }
}

fn close_image_subscriber(cell: &ImageSubscriberCell) {
    if let Ok(mut endpoint) = cell.lock() {
        endpoint.take();
    }
}

fn close_pointcloud_subscriber(cell: &PointCloudSubscriberCell) {
    if let Ok(mut endpoint) = cell.lock() {
        endpoint.take();
    }
}

fn stamped_application_len(total_len: usize) -> PyResult<usize> {
    total_len.checked_sub(TIMING_PREFIX_LEN).ok_or_else(|| {
        InvalidPayloadError::new_err(format!(
            "StampedBytes payload is shorter than its {TIMING_PREFIX_LEN}-byte timing prefix"
        ))
    })
}

fn stamped_application_parts(address: usize, total_len: usize) -> PyResult<(usize, usize)> {
    let application_address = address
        .checked_add(TIMING_PREFIX_LEN)
        .ok_or_else(|| InvalidPayloadError::new_err("StampedBytes application address overflow"))?;
    let application_len = stamped_application_len(total_len)?;
    application_address
        .checked_add(application_len)
        .ok_or_else(|| InvalidPayloadError::new_err("StampedBytes application range overflow"))?;
    Ok((application_address, application_len))
}

fn validate_stamped_timing_prefix(prefix: &[u8]) -> PyResult<()> {
    decode_timing(prefix).map(|_| ()).map_err(|error| {
        InvalidPayloadError::new_err(format!("StampedBytes timing prefix is invalid: {error}"))
    })
}

fn take_stamped_bytes_loan_for_send(
    shared: &Arc<LoanShared<StampedBytes>>,
) -> PyResult<rbt_iox2::LoanedSliceSample<StampedBytes>> {
    let mut state = lock_loan_state(shared)?;
    match &*state {
        LoanState::Active {
            sample, mappings, ..
        } => {
            if *mappings != 0 {
                return Err(LoanStillMappedError::new_err(
                    "publisher loan still has live writable mappings",
                ));
            }
            let payload = sample.payload();
            stamped_application_len(payload.len())?;
            validate_stamped_timing_prefix(&payload[..TIMING_PREFIX_LEN])?;
        }
        LoanState::Consumed => {
            return Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            ));
        }
    }

    match std::mem::replace(&mut *state, LoanState::Consumed) {
        LoanState::Active { sample, .. } => Ok(sample),
        LoanState::Consumed => Err(LoanConsumedError::new_err(
            "publisher loan has already been consumed",
        )),
    }
}

/// The only state shared by a public loan handle, its lease roots, and send.
///
/// `Active` owns the initialized rbt_iox2 loan by value.  `address`, `len`,
/// and `metadata` are captured before the first lease is created, allowing
/// all mapped operations to avoid dereferencing the Rust loan.  Replacing the
/// entire state with `Consumed` under this same mutex makes send and new
/// mapping creation mutually exclusive.
enum LoanState<M>
where
    M: rbt_types::SliceMessageAbi,
    M::Header: iceoryx2::prelude::ZeroCopySend,
    M::Element: iceoryx2::prelude::ZeroCopySend,
{
    Active {
        sample: rbt_iox2::LoanedSliceSample<M>,
        address: usize,
        len: usize,
        mappings: usize,
        metadata: LoanMetadata,
    },
    Consumed,
}

struct LoanShared<M>
where
    M: rbt_types::SliceMessageAbi,
    M::Header: iceoryx2::prelude::ZeroCopySend,
    M::Element: iceoryx2::prelude::ZeroCopySend,
{
    state: Mutex<LoanState<M>>,
    guard: Option<Arc<ActivationGuard>>,
}

fn lock_loan_state<'a, M>(shared: &'a Arc<LoanShared<M>>) -> PyResult<MutexGuard<'a, LoanState<M>>>
where
    M: rbt_types::SliceMessageAbi,
    M::Header: iceoryx2::prelude::ZeroCopySend,
    M::Element: iceoryx2::prelude::ZeroCopySend,
{
    shared
        .state
        .lock()
        .map_err(|_| RbtError::new_err("publisher loan state lock was poisoned"))
}

fn take_loan_for_send<M>(shared: &Arc<LoanShared<M>>) -> PyResult<rbt_iox2::LoanedSliceSample<M>>
where
    M: rbt_types::SliceMessageAbi,
    M::Header: iceoryx2::prelude::ZeroCopySend,
    M::Element: iceoryx2::prelude::ZeroCopySend,
{
    if let Some(guard) = &shared.guard
        && guard.is_expired()
    {
        // Expiry revokes send authority but must not drop the native loan:
        // a previously exported NumPy/memoryview lease may still point at
        // this allocation. Ordinary owner/lease release drops the unsent
        // loan only after the final mapping is gone.
        return Err(ActivationExpiredError::new_err(format!(
            "activation {} for process {} has expired",
            guard.activation(),
            guard.process_instance(),
        )));
    }
    let mut state = lock_loan_state(shared)?;
    if let LoanState::Active { mappings, .. } = &*state
        && *mappings != 0
    {
        return Err(LoanStillMappedError::new_err(
            "publisher loan still has live writable mappings",
        ));
    }

    match std::mem::replace(&mut *state, LoanState::Consumed) {
        LoanState::Active { sample, .. } => Ok(sample),
        LoanState::Consumed => Err(LoanConsumedError::new_err(
            "publisher loan has already been consumed",
        )),
    }
}

fn active_loan_address<M>(shared: &Arc<LoanShared<M>>) -> PyResult<usize>
where
    M: rbt_types::SliceMessageAbi,
    M::Header: iceoryx2::prelude::ZeroCopySend,
    M::Element: iceoryx2::prelude::ZeroCopySend,
{
    let state = lock_loan_state(shared)?;
    match &*state {
        LoanState::Active { address, .. } => Ok(*address),
        LoanState::Consumed => Err(LoanConsumedError::new_err(
            "publisher loan has already been consumed",
        )),
    }
}

fn active_loan_export_count<M>(shared: &Arc<LoanShared<M>>) -> PyResult<usize>
where
    M: rbt_types::SliceMessageAbi,
    M::Header: iceoryx2::prelude::ZeroCopySend,
    M::Element: iceoryx2::prelude::ZeroCopySend,
{
    let state = lock_loan_state(shared)?;
    Ok(match &*state {
        LoanState::Active { mappings, .. } => *mappings,
        // No lease can remain usable after a successful/attempted send.  A
        // zero count is useful for diagnostics and mirrors the no-live-root
        // invariant without pretending a consumed loan is active.
        LoanState::Consumed => 0,
    })
}

fn create_lease<M>(shared: &Arc<LoanShared<M>>) -> PyResult<(usize, usize)>
where
    M: rbt_types::SliceMessageAbi,
    M::Header: iceoryx2::prelude::ZeroCopySend,
    M::Element: iceoryx2::prelude::ZeroCopySend,
{
    ensure_activation_active(shared.guard.as_ref())?;
    let mut state = lock_loan_state(shared)?;
    match &mut *state {
        LoanState::Active {
            address,
            len,
            mappings,
            ..
        } => {
            *mappings = mappings
                .checked_add(1)
                .ok_or_else(|| RbtError::new_err("publisher writable mapping count overflow"))?;
            Ok((*address, *len))
        }
        LoanState::Consumed => Err(LoanConsumedError::new_err(
            "publisher loan has already been consumed",
        )),
    }
}

fn decrement_lease<M>(shared: &Arc<LoanShared<M>>)
where
    M: rbt_types::SliceMessageAbi,
    M::Header: iceoryx2::prelude::ZeroCopySend,
    M::Element: iceoryx2::prelude::ZeroCopySend,
{
    let Ok(mut state) = shared.state.lock() else {
        return;
    };
    if let LoanState::Active { mappings, .. } = &mut *state {
        debug_assert!(*mappings > 0);
        if *mappings > 0 {
            *mappings -= 1;
        }
    }
}

type ImageLoanShared = LoanShared<Image>;
type PointCloudLoanShared = LoanShared<PointCloud>;

enum ImageLeaseShared {
    Image(Arc<ImageLoanShared>),
    StampedBytes(Arc<LoanShared<StampedBytes>>),
    RpcRequest(Arc<rpc::RequestLoanShared>),
    RpcResponse(Arc<rpc::ResponseLoanShared>),
}

fn image_lease_parts(shared: &ImageLeaseShared) -> PyResult<(usize, usize)> {
    match shared {
        ImageLeaseShared::Image(shared) => {
            let state = lock_loan_state(shared)?;
            match &*state {
                LoanState::Active {
                    mappings,
                    address,
                    len,
                    ..
                } if *mappings > 0 => Ok((*address, *len)),
                LoanState::Active { .. } => Err(RbtError::new_err(
                    "publisher writable lease is no longer mapped",
                )),
                LoanState::Consumed => Err(LoanConsumedError::new_err(
                    "publisher loan has already been consumed",
                )),
            }
        }
        ImageLeaseShared::StampedBytes(shared) => {
            let state = lock_loan_state(shared)?;
            match &*state {
                LoanState::Active {
                    mappings,
                    address,
                    len,
                    ..
                } if *mappings > 0 => stamped_application_parts(*address, *len),
                LoanState::Active { .. } => Err(RbtError::new_err(
                    "publisher writable lease is no longer mapped",
                )),
                LoanState::Consumed => Err(LoanConsumedError::new_err(
                    "publisher loan has already been consumed",
                )),
            }
        }
        ImageLeaseShared::RpcRequest(shared) => {
            rpc::request_lease_is_active(shared)?;
            let state = shared
                .state
                .lock()
                .map_err(|_| RbtError::new_err("RPC request loan state lock was poisoned"))?;
            match &*state {
                rpc::RequestLoanState::Active { address, len, .. } => Ok((*address, *len)),
                rpc::RequestLoanState::Consumed => {
                    Err(RbtError::new_err("RPC request loan was consumed"))
                }
            }
        }
        ImageLeaseShared::RpcResponse(shared) => {
            rpc::response_lease_is_active(shared)?;
            let state = shared
                .state
                .lock()
                .map_err(|_| RbtError::new_err("RPC response loan state lock was poisoned"))?;
            match &*state {
                rpc::ResponseLoanState::Active { address, len, .. } => Ok((*address, *len)),
                rpc::ResponseLoanState::Consumed => {
                    Err(RbtError::new_err("RPC response loan was consumed"))
                }
            }
        }
    }
}

#[pyclass(module = "rbt._rbt", name = "_ImagePublisher")]
struct PyImagePublisher {
    publisher: ImagePublisherCell,
    maximum_payload_bytes: usize,
    guard: Option<Arc<ActivationGuard>>,
}

#[pymethods]
impl PyImagePublisher {
    #[staticmethod]
    #[pyo3(signature = (descriptor_json, maximum_payload_bytes=6_220_800, max_loans=None, max_borrowed=None))]
    fn open(
        descriptor_json: &str,
        maximum_payload_bytes: usize,
        max_loans: Option<usize>,
        max_borrowed: Option<usize>,
    ) -> PyResult<Self> {
        let descriptor = parse_descriptor(descriptor_json)?;
        let advertised = descriptor.resources();
        let resources = StreamResourceConfig::new(
            max_loans.unwrap_or(advertised.max_loans),
            max_borrowed.unwrap_or(advertised.max_borrowed),
        );
        let publisher = SliceStream::<Image>::publisher_with_resources(
            &descriptor,
            maximum_payload_bytes,
            resources,
        )
        .map_err(stream_py_error)?;
        Ok(Self {
            publisher: Arc::new(Mutex::new(Some(publisher))),
            maximum_payload_bytes,
            guard: None,
        })
    }

    #[getter]
    fn maximum_payload_bytes(&self) -> usize {
        self.maximum_payload_bytes
    }

    #[pyo3(signature = (width, height, encoding, stride=None, sequence=0, timestamp_ns=0))]
    fn loan(
        &self,
        width: u32,
        height: u32,
        encoding: u32,
        stride: Option<u32>,
        sequence: u64,
        timestamp_ns: u64,
    ) -> PyResult<PyLoanedImageSample> {
        ensure_activation_active(self.guard.as_ref())?;
        let channels = match encoding {
            1 | 2 => 3_usize,
            3 => 1_usize,
            _ => {
                return Err(UnsupportedPixelFormatError::new_err(format!(
                    "unsupported Image pixel format value {encoding}"
                )));
            }
        };
        let row_bytes = usize::try_from(width)
            .ok()
            .and_then(|width| width.checked_mul(channels))
            .ok_or_else(|| InvalidImageLayoutError::new_err("Image row bytes overflow"))?;
        let stride = stride.unwrap_or_else(|| {
            // A u32 stride is part of the ABI.  Values which do not fit are
            // rejected below instead of being truncated.
            u32::try_from(row_bytes).unwrap_or(u32::MAX)
        });
        if (stride as usize) < row_bytes && width != 0 && height != 0 {
            return Err(InvalidImageLayoutError::new_err(format!(
                "Image stride {stride} is smaller than row width {row_bytes}"
            )));
        }
        if row_bytes > u32::MAX as usize {
            return Err(InvalidImageLayoutError::new_err(
                "Image row bytes exceed the u32 stride ABI",
            ));
        }
        let payload_len = (stride as usize)
            .checked_mul(height as usize)
            .ok_or_else(|| InvalidImageLayoutError::new_err("Image payload length overflow"))?;

        let endpoint = self
            .publisher
            .lock()
            .map_err(|_| RbtError::new_err("Image publisher endpoint lock was poisoned"))?;
        let publisher = endpoint
            .as_ref()
            .ok_or_else(|| ActivationExpiredError::new_err("Image publisher endpoint is closed"))?;
        let mut sample = publisher.loan_slice(payload_len).map_err(stream_py_error)?;
        {
            let header = sample.header_mut();
            header.sequence = sequence;
            header.timestamp_ns = timestamp_ns;
            header.width = width;
            header.height = height;
            header.stride = stride;
            header.encoding = rbt_iox2::PixelFormat::new(encoding);
        }
        let payload = sample.payload();
        let address = payload.as_ptr() as usize;
        let len = payload.len();
        let metadata = LoanMetadata::Image(ImageLoanMetadata {
            sequence,
            timestamp_ns,
            width,
            height,
            stride,
            encoding,
        });
        Ok(PyLoanedImageSample {
            shared: Arc::new(LoanShared {
                state: Mutex::new(LoanState::Active {
                    sample,
                    address,
                    len,
                    mappings: 0,
                    metadata,
                }),
                guard: self.guard.clone(),
            }),
        })
    }

    fn pressure<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        endpoint_pressure(py, &self.publisher, "Image publisher", |publisher| {
            publisher.pressure()
        })
    }
}

impl PyImagePublisher {
    pub(crate) fn from_activation(
        publisher: ImagePublisherCell,
        maximum_payload_bytes: usize,
        guard: Arc<ActivationGuard>,
    ) -> Self {
        Self {
            publisher,
            maximum_payload_bytes,
            guard: Some(guard),
        }
    }
}

#[pyclass(module = "rbt._rbt", name = "_StampedBytesPublisher")]
struct PyStampedBytesPublisher {
    publisher: StampedBytesPublisherCell,
    maximum_payload_bytes: usize,
}

#[pymethods]
impl PyStampedBytesPublisher {
    #[staticmethod]
    #[pyo3(signature = (descriptor_json, maximum_payload_bytes, max_loans=None, max_borrowed=None))]
    fn open(
        descriptor_json: &str,
        maximum_payload_bytes: usize,
        max_loans: Option<usize>,
        max_borrowed: Option<usize>,
    ) -> PyResult<Self> {
        let descriptor = parse_descriptor(descriptor_json)?;
        let advertised = descriptor.resources();
        let resources = StreamResourceConfig::new(
            max_loans.unwrap_or(advertised.max_loans),
            max_borrowed.unwrap_or(advertised.max_borrowed),
        );
        let publisher = SliceStream::<StampedBytes>::publisher_with_resources(
            &descriptor,
            maximum_payload_bytes,
            resources,
        )
        .map_err(stream_py_error)?;
        Ok(Self {
            publisher: Arc::new(Mutex::new(Some(publisher))),
            maximum_payload_bytes,
        })
    }

    #[getter]
    fn maximum_payload_bytes(&self) -> usize {
        self.maximum_payload_bytes
    }

    fn loan(
        &self,
        sequence: u64,
        application_payload_bytes: usize,
    ) -> PyResult<PyStampedBytesLoan> {
        let total_len = application_payload_bytes
            .checked_add(TIMING_PREFIX_LEN)
            .ok_or_else(|| InvalidPayloadError::new_err("StampedBytes payload length overflow"))?;
        if total_len > self.maximum_payload_bytes {
            return Err(InvalidPayloadError::new_err(format!(
                "StampedBytes payload is {total_len} bytes, above maximum {}",
                self.maximum_payload_bytes
            )));
        }
        let endpoint = self
            .publisher
            .lock()
            .map_err(|_| RbtError::new_err("StampedBytes publisher endpoint lock was poisoned"))?;
        let publisher = endpoint.as_ref().ok_or_else(|| {
            ActivationExpiredError::new_err("StampedBytes publisher endpoint is closed")
        })?;
        let mut sample = publisher.loan_slice(total_len).map_err(stream_py_error)?;
        *sample.header_mut() = sequence;
        let payload = sample.payload();
        let address = payload.as_ptr() as usize;
        let len = payload.len();
        Ok(PyStampedBytesLoan {
            shared: Arc::new(LoanShared {
                state: Mutex::new(LoanState::Active {
                    sample,
                    address,
                    len,
                    mappings: 0,
                    metadata: LoanMetadata::StampedBytes { sequence },
                }),
                guard: None,
            }),
        })
    }

    fn pressure<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        endpoint_pressure(py, &self.publisher, "StampedBytes publisher", |publisher| {
            publisher.pressure()
        })
    }

    fn close(&self) {
        if let Ok(mut endpoint) = self.publisher.lock() {
            endpoint.take();
        }
    }
}

#[pyclass(module = "rbt._rbt", name = "_StampedBytesLoan")]
struct PyStampedBytesLoan {
    shared: Arc<LoanShared<StampedBytes>>,
}

#[pymethods]
impl PyStampedBytesLoan {
    #[getter]
    fn sequence(&self) -> PyResult<u64> {
        let state = lock_loan_state(&self.shared)?;
        match &*state {
            LoanState::Active {
                metadata: LoanMetadata::StampedBytes { sequence },
                ..
            } => Ok(*sequence),
            LoanState::Active { .. } => Err(RbtError::new_err(
                "publisher loan metadata type did not match StampedBytes",
            )),
            LoanState::Consumed => Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            )),
        }
    }

    #[getter]
    fn payload_nbytes(&self) -> PyResult<usize> {
        let state = lock_loan_state(&self.shared)?;
        match &*state {
            LoanState::Active { len, .. } => stamped_application_len(*len),
            LoanState::Consumed => Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            )),
        }
    }

    fn _set_timing_prefix(&self, prefix: Vec<u8>) -> PyResult<()> {
        if prefix.len() != TIMING_PREFIX_LEN {
            return Err(InvalidPayloadError::new_err(format!(
                "StampedBytes timing prefix must be {TIMING_PREFIX_LEN} bytes, got {}",
                prefix.len()
            )));
        }
        validate_stamped_timing_prefix(&prefix)?;
        let mut state = lock_loan_state(&self.shared)?;
        match &mut *state {
            LoanState::Active {
                sample, mappings, ..
            } => {
                if *mappings != 0 {
                    return Err(LoanStillMappedError::new_err(
                        "publisher loan still has live writable mappings",
                    ));
                }
                stamped_application_len(sample.payload().len())?;
                sample.payload_mut()[..TIMING_PREFIX_LEN].copy_from_slice(&prefix);
                Ok(())
            }
            LoanState::Consumed => Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            )),
        }
    }

    fn _new_lease(&self) -> PyResult<PyImageLease> {
        let (address, len) = create_lease(&self.shared)?;
        let address = match stamped_application_parts(address, len) {
            Ok((address, _)) => address,
            Err(error) => {
                decrement_lease(&self.shared);
                return Err(error);
            }
        };
        Ok(PyImageLease {
            shared: ImageLeaseShared::StampedBytes(Arc::clone(&self.shared)),
            address,
        })
    }

    fn send(&self) -> PyResult<()> {
        let sample = take_stamped_bytes_loan_for_send(&self.shared)?;
        sample.send().map(|_| ()).map_err(stream_py_error)
    }

    fn _debug_payload_address(&self) -> PyResult<usize> {
        let state = lock_loan_state(&self.shared)?;
        match &*state {
            LoanState::Active { address, len, .. } => {
                Ok(stamped_application_parts(*address, *len)?.0)
            }
            LoanState::Consumed => Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            )),
        }
    }

    fn _debug_export_count(&self) -> PyResult<usize> {
        active_loan_export_count(&self.shared)
    }
}

#[pyclass(module = "rbt._rbt", name = "_PointCloudPublisher")]
struct PyPointCloudPublisher {
    publisher: PointCloudPublisherCell,
    maximum_points: usize,
    guard: Option<Arc<ActivationGuard>>,
}

#[pymethods]
impl PyPointCloudPublisher {
    #[staticmethod]
    #[pyo3(signature = (descriptor_json, maximum_points=100_000, max_loans=None, max_borrowed=None))]
    fn open(
        descriptor_json: &str,
        maximum_points: usize,
        max_loans: Option<usize>,
        max_borrowed: Option<usize>,
    ) -> PyResult<Self> {
        let descriptor = parse_descriptor(descriptor_json)?;
        let advertised = descriptor.resources();
        let resources = StreamResourceConfig::new(
            max_loans.unwrap_or(advertised.max_loans),
            max_borrowed.unwrap_or(advertised.max_borrowed),
        );
        let publisher = SliceStream::<PointCloud>::publisher_with_resources(
            &descriptor,
            maximum_points,
            resources,
        )
        .map_err(stream_py_error)?;
        Ok(Self {
            publisher: Arc::new(Mutex::new(Some(publisher))),
            maximum_points,
            guard: None,
        })
    }

    #[getter]
    fn maximum_points(&self) -> usize {
        self.maximum_points
    }

    #[pyo3(signature = (num_points, sequence=0, timestamp_ns=0, dense=true))]
    fn loan(
        &self,
        num_points: usize,
        sequence: u64,
        timestamp_ns: u64,
        dense: bool,
    ) -> PyResult<PyLoanedPointCloudSample> {
        ensure_activation_active(self.guard.as_ref())?;
        if num_points > u32::MAX as usize {
            return Err(InvalidPayloadError::new_err(
                "PointCloud point count exceeds the u32 width ABI",
            ));
        }
        let endpoint = self
            .publisher
            .lock()
            .map_err(|_| RbtError::new_err("PointCloud publisher endpoint lock was poisoned"))?;
        let publisher = endpoint.as_ref().ok_or_else(|| {
            ActivationExpiredError::new_err("PointCloud publisher endpoint is closed")
        })?;
        let mut sample = publisher.loan_slice(num_points).map_err(stream_py_error)?;
        {
            let header = sample.header_mut();
            header.sequence = sequence;
            header.timestamp_ns = timestamp_ns;
            header.width = num_points as u32;
            header.height = 1;
            header.dense = u8::from(dense);
            header.reserved = [0; 7];
        }
        let payload = sample.payload();
        let address = payload.as_ptr() as usize;
        let len = payload
            .len()
            .checked_mul(size_of::<rbt_iox2::PointXYZI>())
            .ok_or_else(|| InvalidPayloadError::new_err("PointCloud payload length overflow"))?;
        let metadata = LoanMetadata::PointCloud(PointCloudLoanMetadata {
            sequence,
            timestamp_ns,
            width: num_points as u32,
            height: 1,
            dense,
            point_count: num_points,
        });
        Ok(PyLoanedPointCloudSample {
            shared: Arc::new(LoanShared {
                state: Mutex::new(LoanState::Active {
                    sample,
                    address,
                    len,
                    mappings: 0,
                    metadata,
                }),
                guard: self.guard.clone(),
            }),
        })
    }

    fn pressure<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        endpoint_pressure(py, &self.publisher, "PointCloud publisher", |publisher| {
            publisher.pressure()
        })
    }
}

impl PyPointCloudPublisher {
    pub(crate) fn from_activation(
        publisher: PointCloudPublisherCell,
        maximum_points: usize,
        guard: Arc<ActivationGuard>,
    ) -> Self {
        Self {
            publisher,
            maximum_points,
            guard: Some(guard),
        }
    }
}

#[pyclass(module = "rbt._rbt", name = "_LoanedImageSample")]
struct PyLoanedImageSample {
    shared: Arc<ImageLoanShared>,
}

impl PyLoanedImageSample {
    fn metadata(&self) -> PyResult<ImageLoanMetadata> {
        let state = lock_loan_state(&self.shared)?;
        match &*state {
            LoanState::Active {
                metadata: LoanMetadata::Image(metadata),
                ..
            } => Ok(*metadata),
            LoanState::Active { .. } => Err(RbtError::new_err(
                "publisher loan metadata type did not match Image",
            )),
            LoanState::Consumed => Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            )),
        }
    }
}

#[pymethods]
impl PyLoanedImageSample {
    #[getter]
    fn sequence(&self) -> PyResult<u64> {
        Ok(self.metadata()?.sequence)
    }

    #[getter]
    fn timestamp_ns(&self) -> PyResult<u64> {
        Ok(self.metadata()?.timestamp_ns)
    }

    #[getter]
    fn width(&self) -> PyResult<u32> {
        Ok(self.metadata()?.width)
    }

    #[getter]
    fn height(&self) -> PyResult<u32> {
        Ok(self.metadata()?.height)
    }

    #[getter]
    fn stride(&self) -> PyResult<u32> {
        Ok(self.metadata()?.stride)
    }

    #[getter]
    fn encoding(&self) -> PyResult<u32> {
        Ok(self.metadata()?.encoding)
    }

    #[getter]
    fn payload_nbytes(&self) -> PyResult<usize> {
        let state = lock_loan_state(&self.shared)?;
        match &*state {
            LoanState::Active { len, .. } => Ok(*len),
            LoanState::Consumed => Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            )),
        }
    }

    fn _new_lease(&self) -> PyResult<PyImageLease> {
        let (address, _) = create_lease(&self.shared)?;
        Ok(PyImageLease {
            shared: ImageLeaseShared::Image(Arc::clone(&self.shared)),
            address,
        })
    }

    fn send(&self) -> PyResult<()> {
        let sample = take_loan_for_send(&self.shared)?;
        sample.send().map(|_| ()).map_err(stream_py_error)
    }

    fn _debug_payload_address(&self) -> PyResult<usize> {
        active_loan_address(&self.shared)
    }

    fn _debug_export_count(&self) -> PyResult<usize> {
        active_loan_export_count(&self.shared)
    }
}

#[pyclass(module = "rbt._rbt", name = "_ImageBufferLease")]
pub(crate) struct PyImageLease {
    shared: ImageLeaseShared,
    address: usize,
}

impl PyImageLease {
    pub(crate) fn from_rpc_request(shared: Arc<rpc::RequestLoanShared>, address: usize) -> Self {
        Self {
            shared: ImageLeaseShared::RpcRequest(shared),
            address,
        }
    }

    pub(crate) fn from_rpc_response(shared: Arc<rpc::ResponseLoanShared>, address: usize) -> Self {
        Self {
            shared: ImageLeaseShared::RpcResponse(shared),
            address,
        }
    }
}

impl Drop for PyImageLease {
    fn drop(&mut self) {
        match &self.shared {
            ImageLeaseShared::Image(shared) => decrement_lease(shared),
            ImageLeaseShared::StampedBytes(shared) => decrement_lease(shared),
            ImageLeaseShared::RpcRequest(shared) => rpc::decrement_request_lease(shared),
            ImageLeaseShared::RpcResponse(shared) => rpc::decrement_response_lease(shared),
        }
    }
}

#[pymethods]
impl PyImageLease {
    fn _debug_buffer_address(&self) -> usize {
        self.address
    }

    /// Exports the cached initialized Image payload as a writable byte buffer.
    /// The mapping count was incremented when this lease was created, not by
    /// this transient CPython buffer request.
    unsafe fn __getbuffer__(
        slf: Bound<'_, Self>,
        view: *mut ffi::Py_buffer,
        flags: c_int,
    ) -> PyResult<()> {
        if view.is_null() {
            return Err(PyBufferError::new_err("buffer view is null"));
        }
        let (address, len) = {
            let lease = slf.borrow();
            image_lease_parts(&lease.shared)?
        };
        // SAFETY: the lease keeps the Active sample alive and the CPython
        // buffer owns a strong reference to this exact lease.
        unsafe { fill_writable_buffer(view, flags, address, len, slf.into_any()) }
    }
}

#[pyclass(module = "rbt._rbt", name = "_LoanedPointCloudSample")]
struct PyLoanedPointCloudSample {
    shared: Arc<PointCloudLoanShared>,
}

impl PyLoanedPointCloudSample {
    fn metadata(&self) -> PyResult<PointCloudLoanMetadata> {
        let state = lock_loan_state(&self.shared)?;
        match &*state {
            LoanState::Active {
                metadata: LoanMetadata::PointCloud(metadata),
                ..
            } => Ok(*metadata),
            LoanState::Active { .. } => Err(RbtError::new_err(
                "publisher loan metadata type did not match PointCloud",
            )),
            LoanState::Consumed => Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            )),
        }
    }
}

#[pymethods]
impl PyLoanedPointCloudSample {
    #[getter]
    fn sequence(&self) -> PyResult<u64> {
        Ok(self.metadata()?.sequence)
    }

    #[getter]
    fn timestamp_ns(&self) -> PyResult<u64> {
        Ok(self.metadata()?.timestamp_ns)
    }

    #[getter]
    fn width(&self) -> PyResult<u32> {
        Ok(self.metadata()?.width)
    }

    #[getter]
    fn height(&self) -> PyResult<u32> {
        Ok(self.metadata()?.height)
    }

    #[getter]
    fn dense(&self) -> PyResult<bool> {
        Ok(self.metadata()?.dense)
    }

    #[getter]
    fn point_count(&self) -> PyResult<usize> {
        Ok(self.metadata()?.point_count)
    }

    #[getter]
    fn payload_nbytes(&self) -> PyResult<usize> {
        let state = lock_loan_state(&self.shared)?;
        match &*state {
            LoanState::Active { len, .. } => Ok(*len),
            LoanState::Consumed => Err(LoanConsumedError::new_err(
                "publisher loan has already been consumed",
            )),
        }
    }

    #[getter]
    fn payload_alignment(&self) -> usize {
        align_of::<rbt_iox2::PointXYZI>()
    }

    fn _new_lease(&self) -> PyResult<PyPointCloudLease> {
        let (address, len) = create_lease(&self.shared)?;
        Ok(PyPointCloudLease {
            shared: Arc::clone(&self.shared),
            address,
            len,
        })
    }

    fn send(&self) -> PyResult<()> {
        let sample = take_loan_for_send(&self.shared)?;
        sample.send().map(|_| ()).map_err(stream_py_error)
    }

    fn _debug_payload_address(&self) -> PyResult<usize> {
        active_loan_address(&self.shared)
    }

    fn _debug_export_count(&self) -> PyResult<usize> {
        active_loan_export_count(&self.shared)
    }
}

#[pyclass(module = "rbt._rbt", name = "_PointCloudBufferLease")]
struct PyPointCloudLease {
    shared: Arc<PointCloudLoanShared>,
    address: usize,
    len: usize,
}

impl Drop for PyPointCloudLease {
    fn drop(&mut self) {
        decrement_lease(&self.shared);
    }
}

#[pymethods]
impl PyPointCloudLease {
    fn _debug_buffer_address(&self) -> usize {
        self.address
    }

    /// Exports the cached initialized PointXYZI payload as writable bytes.
    unsafe fn __getbuffer__(
        slf: Bound<'_, Self>,
        view: *mut ffi::Py_buffer,
        flags: c_int,
    ) -> PyResult<()> {
        if view.is_null() {
            return Err(PyBufferError::new_err("buffer view is null"));
        }
        let (address, len) = {
            let lease = slf.borrow();
            let state = lock_loan_state(&lease.shared)?;
            match &*state {
                LoanState::Active { mappings, .. } if *mappings > 0 => (lease.address, lease.len),
                LoanState::Active { .. } => {
                    return Err(RbtError::new_err(
                        "publisher writable lease is no longer mapped",
                    ));
                }
                LoanState::Consumed => {
                    return Err(LoanConsumedError::new_err(
                        "publisher loan has already been consumed",
                    ));
                }
            }
        };
        // SAFETY: the lease keeps the Active sample alive and the CPython
        // buffer owns a strong reference to this exact lease.
        unsafe { fill_writable_buffer(view, flags, address, len, slf.into_any()) }
    }
}

enum ReadOnlyImageOwner {
    Image(Mutex<ReceivedSliceSample<Image>>),
    StampedBytes(Arc<Mutex<ReceivedSliceSample<StampedBytes>>>),
    StampedTimingPrefix(Arc<Mutex<ReceivedSliceSample<StampedBytes>>>),
    RpcRequest(Arc<rpc::RequestShared>),
    RpcResponse(Arc<rpc::RpcResponseGuardShared>),
}

#[pyclass(module = "rbt._rbt", name = "_ReceivedImageSample")]
pub(crate) struct PyReceivedImageSample {
    // A NumPy view owns the received shared-memory sample for as long as the view or any derived view can still access that memory.
    // `ReceivedSliceSample` is Send but deliberately not Sync because it
    // contains raw SHM pointers. The mutex supplies the synchronization PyO3
    // requires while still allowing the guard to be dropped on any thread.
    owner: ReadOnlyImageOwner,
}

impl PyReceivedImageSample {
    pub(crate) fn from_rpc_request(shared: Arc<rpc::RequestShared>) -> Self {
        Self {
            owner: ReadOnlyImageOwner::RpcRequest(shared),
        }
    }

    pub(crate) fn from_rpc_response(shared: Arc<rpc::RpcResponseGuardShared>) -> Self {
        Self {
            owner: ReadOnlyImageOwner::RpcResponse(shared),
        }
    }

    fn metadata(&self) -> PyResult<ImageHeader> {
        match &self.owner {
            ReadOnlyImageOwner::Image(sample) => Ok(*lock_sample(sample)?.header()),
            ReadOnlyImageOwner::StampedBytes(_) | ReadOnlyImageOwner::StampedTimingPrefix(_) => {
                Err(InvalidPayloadError::new_err(
                    "StampedBytes does not contain an Image header",
                ))
            }
            ReadOnlyImageOwner::RpcRequest(shared) => rpc::request_image_header(shared),
            ReadOnlyImageOwner::RpcResponse(shared) => rpc::response_guard_image_header(shared),
        }
    }
}

#[pymethods]
impl PyReceivedImageSample {
    #[getter]
    fn sequence(&self) -> PyResult<u64> {
        match &self.owner {
            ReadOnlyImageOwner::StampedBytes(sample)
            | ReadOnlyImageOwner::StampedTimingPrefix(sample) => Ok(*lock_sample(sample)?.header()),
            _ => Ok(self.metadata()?.sequence),
        }
    }

    #[getter]
    fn timestamp_ns(&self) -> PyResult<u64> {
        Ok(self.metadata()?.timestamp_ns)
    }

    #[getter]
    fn width(&self) -> PyResult<u32> {
        Ok(self.metadata()?.width)
    }

    #[getter]
    fn height(&self) -> PyResult<u32> {
        Ok(self.metadata()?.height)
    }

    #[getter]
    fn stride(&self) -> PyResult<u32> {
        Ok(self.metadata()?.stride)
    }

    #[getter]
    fn encoding(&self) -> PyResult<u32> {
        Ok(self.metadata()?.encoding.value())
    }

    #[getter]
    fn payload_nbytes(&self) -> PyResult<usize> {
        match &self.owner {
            ReadOnlyImageOwner::Image(sample) => Ok(lock_sample(sample)?.payload().len()),
            ReadOnlyImageOwner::StampedBytes(sample) => {
                stamped_application_len(lock_sample(sample)?.payload().len())
            }
            ReadOnlyImageOwner::StampedTimingPrefix(sample) => {
                stamped_application_len(lock_sample(sample)?.payload().len())?;
                Ok(TIMING_PREFIX_LEN)
            }
            ReadOnlyImageOwner::RpcRequest(shared) => Ok(rpc::request_payload_parts(shared)?.1),
            ReadOnlyImageOwner::RpcResponse(shared) => Ok(shared.len),
        }
    }

    fn _debug_payload_address(&self) -> PyResult<usize> {
        match &self.owner {
            ReadOnlyImageOwner::Image(sample) => {
                Ok(lock_sample(sample)?.payload().as_ptr() as usize)
            }
            ReadOnlyImageOwner::StampedBytes(sample) => {
                let sample = lock_sample(sample)?;
                let payload = sample.payload();
                stamped_application_len(payload.len())?;
                Ok(payload.as_ptr().wrapping_add(TIMING_PREFIX_LEN) as usize)
            }
            ReadOnlyImageOwner::StampedTimingPrefix(sample) => {
                let sample = lock_sample(sample)?;
                stamped_application_len(sample.payload().len())?;
                Ok(sample.payload().as_ptr() as usize)
            }
            ReadOnlyImageOwner::RpcRequest(shared) => Ok(rpc::request_payload_parts(shared)?.0),
            ReadOnlyImageOwner::RpcResponse(shared) => Ok(shared.address),
        }
    }

    fn _timing_prefix(&self) -> PyResult<Self> {
        let owner = match &self.owner {
            ReadOnlyImageOwner::StampedBytes(sample)
            | ReadOnlyImageOwner::StampedTimingPrefix(sample) => sample,
            _ => {
                return Err(InvalidPayloadError::new_err(
                    "timing prefix is available only for StampedBytes samples",
                ));
            }
        };
        stamped_application_len(lock_sample(owner)?.payload().len())?;
        Ok(Self {
            owner: ReadOnlyImageOwner::StampedTimingPrefix(Arc::clone(owner)),
        })
    }

    fn __repr__(&self) -> PyResult<String> {
        if matches!(
            &self.owner,
            ReadOnlyImageOwner::StampedBytes(_) | ReadOnlyImageOwner::StampedTimingPrefix(_)
        ) {
            Ok(format!(
                "<ReceivedStampedBytesSample seq={} payload_nbytes={} borrowed=True>",
                self.sequence()?,
                self.payload_nbytes()?
            ))
        } else {
            Ok(format!(
                "<ReceivedImageSample seq={} shape={}x{} encoding={} borrowed=True>",
                self.sequence()?,
                self.width()?,
                self.height()?,
                self.encoding()?
            ))
        }
    }

    unsafe fn __getbuffer__(
        slf: Bound<'_, Self>,
        view: *mut ffi::Py_buffer,
        flags: c_int,
    ) -> PyResult<()> {
        let (data, len, request_shared) = {
            let sample = slf.borrow();
            match &sample.owner {
                ReadOnlyImageOwner::Image(owner) => {
                    let sample = lock_sample(owner)?;
                    (sample.payload().as_ptr(), sample.payload().len(), None)
                }
                ReadOnlyImageOwner::StampedBytes(owner) => {
                    let sample = lock_sample(owner)?;
                    let payload = sample.payload();
                    let len = stamped_application_len(payload.len())?;
                    (payload.as_ptr().wrapping_add(TIMING_PREFIX_LEN), len, None)
                }
                ReadOnlyImageOwner::StampedTimingPrefix(owner) => {
                    let sample = lock_sample(owner)?;
                    let payload = sample.payload();
                    stamped_application_len(payload.len())?;
                    (payload.as_ptr(), TIMING_PREFIX_LEN, None)
                }
                ReadOnlyImageOwner::RpcRequest(shared) => {
                    let (address, len) = rpc::begin_request_borrow(shared)?;
                    (address as *const u8, len, Some(Arc::clone(shared)))
                }
                ReadOnlyImageOwner::RpcResponse(shared) => {
                    (shared.address as *const u8, shared.len, None)
                }
            }
        };
        // SAFETY: `data` belongs to the sample stored in `slf`; transferring
        // `slf` as owner makes the buffer lifetime a strict subset of it.
        let result = unsafe { fill_readonly_buffer(view, flags, data, len, slf.into_any()) };
        if result.is_err()
            && let Some(shared) = request_shared
        {
            rpc::end_request_borrow(&shared);
        }
        result
    }

    unsafe fn __releasebuffer__(&self, view: *mut ffi::Py_buffer) {
        if let ReadOnlyImageOwner::RpcRequest(shared) = &self.owner {
            rpc::end_request_borrow(shared);
        }
        // SAFETY: paired with this class's `__getbuffer__` implementation.
        unsafe { release_readonly_buffer(view) }
    }
}

#[pyclass(module = "rbt._rbt", name = "_ReceivedPointCloudSample")]
struct PyReceivedPointCloudSample {
    sample: Mutex<ReceivedSliceSample<PointCloud>>,
}

#[pymethods]
impl PyReceivedPointCloudSample {
    #[getter]
    fn sequence(&self) -> PyResult<u64> {
        Ok(lock_sample(&self.sample)?.header().sequence)
    }

    #[getter]
    fn timestamp_ns(&self) -> PyResult<u64> {
        Ok(lock_sample(&self.sample)?.header().timestamp_ns)
    }

    #[getter]
    fn width(&self) -> PyResult<u32> {
        Ok(lock_sample(&self.sample)?.header().width)
    }

    #[getter]
    fn height(&self) -> PyResult<u32> {
        Ok(lock_sample(&self.sample)?.header().height)
    }

    #[getter]
    fn dense(&self) -> PyResult<bool> {
        Ok(lock_sample(&self.sample)?.header().dense != 0)
    }

    #[getter]
    fn point_count(&self) -> PyResult<usize> {
        Ok(lock_sample(&self.sample)?.payload().len())
    }

    #[getter]
    fn payload_nbytes(&self) -> PyResult<usize> {
        Ok(size_of_val(lock_sample(&self.sample)?.payload()))
    }

    #[getter]
    fn payload_alignment(&self) -> usize {
        align_of::<rbt_iox2::PointXYZI>()
    }

    fn _debug_payload_address(&self) -> PyResult<usize> {
        Ok(lock_sample(&self.sample)?.payload().as_ptr() as usize)
    }

    fn __repr__(&self) -> PyResult<String> {
        Ok(format!(
            "<ReceivedPointCloudSample seq={} points={} borrowed=True>",
            self.sequence()?,
            self.point_count()?
        ))
    }

    unsafe fn __getbuffer__(
        slf: Bound<'_, Self>,
        view: *mut ffi::Py_buffer,
        flags: c_int,
    ) -> PyResult<()> {
        let (data, len) = {
            let sample = slf.borrow();
            let sample = lock_sample(&sample.sample)?;
            let payload = sample.payload();
            (payload.as_ptr().cast::<u8>(), size_of_val(payload))
        };
        // SAFETY: `data` belongs to the sample stored in `slf`; transferring
        // `slf` as owner makes the buffer lifetime a strict subset of it.
        unsafe { fill_readonly_buffer(view, flags, data, len, slf.into_any()) }
    }

    unsafe fn __releasebuffer__(&self, view: *mut ffi::Py_buffer) {
        // SAFETY: paired with this class's `__getbuffer__` implementation.
        unsafe { release_readonly_buffer(view) }
    }
}

#[pyclass(module = "rbt._rbt", name = "_ImageSubscriber")]
struct PyImageSubscriber {
    subscriber: ImageSubscriberCell,
    process_instance: ProcessInstanceId,
    next_activation: u64,
    wait_stats: CumulativeWaitStats,
    guard: Option<Arc<ActivationGuard>>,
    async_state: Option<Arc<AsyncSubscriberState>>,
    _async_bridge: Option<Arc<AsyncActivationBridge>>,
}

#[pymethods]
impl PyImageSubscriber {
    #[staticmethod]
    #[pyo3(signature = (descriptor_json, max_loans=None, max_borrowed=None))]
    fn open(
        descriptor_json: &str,
        max_loans: Option<usize>,
        max_borrowed: Option<usize>,
    ) -> PyResult<Self> {
        let descriptor = parse_descriptor(descriptor_json)?;
        let advertised = descriptor.resources();
        let resources = StreamResourceConfig::new(
            max_loans.unwrap_or(advertised.max_loans),
            max_borrowed.unwrap_or(advertised.max_borrowed),
        );
        let subscriber = SliceStream::<Image>::subscriber_with_resources(&descriptor, resources)
            .map_err(stream_py_error)?;
        let process_instance = ProcessInstanceId::new();
        Ok(Self {
            subscriber: Arc::new(Mutex::new(Some(subscriber))),
            process_instance,
            next_activation: 1,
            wait_stats: CumulativeWaitStats::default(),
            guard: None,
            async_state: None,
            _async_bridge: None,
        })
    }

    fn try_receive(&self) -> PyResult<Option<PyReceivedImageSample>> {
        ensure_activation_active(self.guard.as_ref())?;
        let endpoint = self
            .subscriber
            .lock()
            .map_err(|_| RbtError::new_err("Image subscriber endpoint lock was poisoned"))?;
        let subscriber = endpoint.as_ref().ok_or_else(|| {
            ActivationExpiredError::new_err("Image subscriber endpoint is closed")
        })?;
        subscriber
            .receive()
            .map(|sample| {
                sample.map(|sample| PyReceivedImageSample {
                    owner: ReadOnlyImageOwner::Image(Mutex::new(sample)),
                })
            })
            .map_err(|error| {
                stream_py_error_with_borrow_limit(
                    error,
                    subscriber.pressure().resources.max_borrowed,
                )
            })
    }

    fn wait_receive(
        &mut self,
        py: Python<'_>,
        timeout_seconds: f64,
    ) -> PyResult<Option<PyReceivedImageSample>> {
        ensure_activation_active(self.guard.as_ref())?;
        let timeout = checked_timeout(timeout_seconds)?;
        let (process_instance, activation) = if let Some(guard) = &self.guard {
            (guard.process_instance(), guard.activation())
        } else {
            let activation = ActivationId::new(self.next_activation);
            self.next_activation = self.next_activation.saturating_add(1);
            (self.process_instance, activation)
        };
        let subscriber_cell = Arc::clone(&self.subscriber);
        let guard = self.guard.clone();
        let outcome = py
            .detach(move || {
                let endpoint = subscriber_cell.lock().map_err(|_| {
                    ReceiveError::Wait(NativeWaitError::ListenerWait(
                        "Image subscriber endpoint lock was poisoned".to_owned(),
                    ))
                })?;
                let subscriber = endpoint.as_ref().ok_or_else(|| {
                    ReceiveError::Wait(NativeWaitError::ListenerWait(
                        "Image subscriber endpoint is closed".to_owned(),
                    ))
                })?;
                wait_for_slice(
                    subscriber,
                    process_instance,
                    activation,
                    timeout,
                    guard.as_ref(),
                )
            })
            .map_err(receive_py_error)?;
        self.wait_stats.add(outcome.stats);
        py.check_signals()?;
        Ok(outcome.sample.map(|sample| PyReceivedImageSample {
            owner: ReadOnlyImageOwner::Image(Mutex::new(sample)),
        }))
    }

    fn pressure<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        endpoint_pressure(py, &self.subscriber, "Image subscriber", |subscriber| {
            subscriber.pressure()
        })
    }

    fn wait_stats(&self) -> (u64, u64, u64, u64, u64) {
        let stats = self.wait_stats;
        (
            stats.waits,
            stats.wakeups,
            stats.empty_wakeups,
            stats.cancellation_wakeups,
            stats.samples_processed,
        )
    }

    fn activation_cancelled(&self) -> bool {
        self.guard.as_ref().is_some_and(|guard| guard.is_expired())
    }

    fn async_receive_supported(&self) -> bool {
        self.async_state.is_some()
    }

    fn _debug_async_wait_stats(&self) -> (u64, u64, u64) {
        self.async_state
            .as_ref()
            .map(|state| state.debug_stats())
            .unwrap_or((0, 0, 0))
    }

    #[pyo3(signature = (loop_object, future))]
    fn begin_receive_async(
        &self,
        loop_object: Bound<'_, PyAny>,
        future: Bound<'_, PyAny>,
    ) -> PyResult<u64> {
        ensure_activation_active(self.guard.as_ref())?;
        let state = self.async_state.as_ref().ok_or_else(|| {
            AsyncRuntimeClosedError::new_err(
                "receive_async is available only for activation-bound subscribers",
            )
        })?;
        state.begin(loop_object, future)
    }

    fn finish_receive_async(&self, generation: u64) -> PyResult<()> {
        let Some(state) = self.async_state.as_ref() else {
            return Err(AsyncRuntimeClosedError::new_err(
                "receive_async is available only for activation-bound subscribers",
            ));
        };
        if let Some(error) = state.finish(generation) {
            return Err(async_runtime::bridge_failure(error));
        }
        Ok(())
    }

    fn cancel_receive_async(&self, generation: u64) {
        if let Some(state) = self.async_state.as_ref() {
            state.cancel(generation);
        }
    }
}

impl PyImageSubscriber {
    pub(crate) fn from_activation(
        subscriber: ImageSubscriberCell,
        guard: Arc<ActivationGuard>,
        async_state: Arc<AsyncSubscriberState>,
    ) -> Self {
        let process_instance = guard.process_instance();
        let async_bridge = guard.async_bridge();
        Self {
            subscriber,
            process_instance,
            next_activation: 1,
            wait_stats: CumulativeWaitStats::default(),
            guard: Some(guard),
            async_state: Some(async_state),
            _async_bridge: Some(async_bridge),
        }
    }
}

#[pyclass(module = "rbt._rbt", name = "_StampedBytesSubscriber")]
struct PyStampedBytesSubscriber {
    subscriber: StampedBytesSubscriberCell,
    process_instance: ProcessInstanceId,
    next_activation: u64,
    wait_stats: CumulativeWaitStats,
}

#[pymethods]
impl PyStampedBytesSubscriber {
    #[staticmethod]
    #[pyo3(signature = (descriptor_json, max_loans=None, max_borrowed=None))]
    fn open(
        descriptor_json: &str,
        max_loans: Option<usize>,
        max_borrowed: Option<usize>,
    ) -> PyResult<Self> {
        let descriptor = parse_descriptor(descriptor_json)?;
        let advertised = descriptor.resources();
        let resources = StreamResourceConfig::new(
            max_loans.unwrap_or(advertised.max_loans),
            max_borrowed.unwrap_or(advertised.max_borrowed),
        );
        let subscriber =
            SliceStream::<StampedBytes>::subscriber_with_resources(&descriptor, resources)
                .map_err(stream_py_error)?;
        Ok(Self {
            subscriber: Arc::new(Mutex::new(Some(subscriber))),
            process_instance: ProcessInstanceId::new(),
            next_activation: 1,
            wait_stats: CumulativeWaitStats::default(),
        })
    }

    fn try_receive(&self) -> PyResult<Option<PyReceivedImageSample>> {
        let endpoint = self
            .subscriber
            .lock()
            .map_err(|_| RbtError::new_err("StampedBytes subscriber endpoint lock was poisoned"))?;
        let subscriber = endpoint.as_ref().ok_or_else(|| {
            ActivationExpiredError::new_err("StampedBytes subscriber endpoint is closed")
        })?;
        subscriber
            .receive()
            .map(|sample| {
                sample.map(|sample| PyReceivedImageSample {
                    owner: ReadOnlyImageOwner::StampedBytes(Arc::new(Mutex::new(sample))),
                })
            })
            .map_err(|error| {
                stream_py_error_with_borrow_limit(
                    error,
                    subscriber.pressure().resources.max_borrowed,
                )
            })
    }

    fn wait_receive(
        &mut self,
        py: Python<'_>,
        timeout_seconds: f64,
    ) -> PyResult<Option<PyReceivedImageSample>> {
        let timeout = checked_timeout(timeout_seconds)?;
        let activation = ActivationId::new(self.next_activation);
        self.next_activation = self.next_activation.saturating_add(1);
        let process_instance = self.process_instance;
        let subscriber_cell = Arc::clone(&self.subscriber);
        let outcome = py
            .detach(move || {
                let endpoint = subscriber_cell.lock().map_err(|_| {
                    ReceiveError::Wait(NativeWaitError::ListenerWait(
                        "StampedBytes subscriber endpoint lock was poisoned".to_owned(),
                    ))
                })?;
                let subscriber = endpoint.as_ref().ok_or_else(|| {
                    ReceiveError::Wait(NativeWaitError::ListenerWait(
                        "StampedBytes subscriber endpoint is closed".to_owned(),
                    ))
                })?;
                wait_for_slice(subscriber, process_instance, activation, timeout, None)
            })
            .map_err(receive_py_error)?;
        self.wait_stats.add(outcome.stats);
        py.check_signals()?;
        Ok(outcome.sample.map(|sample| PyReceivedImageSample {
            owner: ReadOnlyImageOwner::StampedBytes(Arc::new(Mutex::new(sample))),
        }))
    }

    fn pressure<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        endpoint_pressure(
            py,
            &self.subscriber,
            "StampedBytes subscriber",
            |subscriber| subscriber.pressure(),
        )
    }

    fn wait_stats(&self) -> (u64, u64, u64, u64, u64) {
        let stats = self.wait_stats;
        (
            stats.waits,
            stats.wakeups,
            stats.empty_wakeups,
            stats.cancellation_wakeups,
            stats.samples_processed,
        )
    }

    fn close(&self) {
        if let Ok(mut endpoint) = self.subscriber.lock() {
            endpoint.take();
        }
    }
}

#[pyclass(module = "rbt._rbt", name = "_PointCloudSubscriber")]
struct PyPointCloudSubscriber {
    subscriber: PointCloudSubscriberCell,
    process_instance: ProcessInstanceId,
    next_activation: u64,
    wait_stats: CumulativeWaitStats,
    guard: Option<Arc<ActivationGuard>>,
    async_state: Option<Arc<AsyncSubscriberState>>,
    _async_bridge: Option<Arc<AsyncActivationBridge>>,
}

#[pymethods]
impl PyPointCloudSubscriber {
    #[staticmethod]
    #[pyo3(signature = (descriptor_json, max_loans=None, max_borrowed=None))]
    fn open(
        descriptor_json: &str,
        max_loans: Option<usize>,
        max_borrowed: Option<usize>,
    ) -> PyResult<Self> {
        let descriptor = parse_descriptor(descriptor_json)?;
        let advertised = descriptor.resources();
        let resources = StreamResourceConfig::new(
            max_loans.unwrap_or(advertised.max_loans),
            max_borrowed.unwrap_or(advertised.max_borrowed),
        );
        let subscriber =
            SliceStream::<PointCloud>::subscriber_with_resources(&descriptor, resources)
                .map_err(stream_py_error)?;
        let process_instance = ProcessInstanceId::new();
        Ok(Self {
            subscriber: Arc::new(Mutex::new(Some(subscriber))),
            process_instance,
            next_activation: 1,
            wait_stats: CumulativeWaitStats::default(),
            guard: None,
            async_state: None,
            _async_bridge: None,
        })
    }

    fn try_receive(&self) -> PyResult<Option<PyReceivedPointCloudSample>> {
        ensure_activation_active(self.guard.as_ref())?;
        let endpoint = self
            .subscriber
            .lock()
            .map_err(|_| RbtError::new_err("PointCloud subscriber endpoint lock was poisoned"))?;
        let subscriber = endpoint.as_ref().ok_or_else(|| {
            ActivationExpiredError::new_err("PointCloud subscriber endpoint is closed")
        })?;
        subscriber
            .receive()
            .map(|sample| {
                sample.map(|sample| PyReceivedPointCloudSample {
                    sample: Mutex::new(sample),
                })
            })
            .map_err(|error| {
                stream_py_error_with_borrow_limit(
                    error,
                    subscriber.pressure().resources.max_borrowed,
                )
            })
    }

    fn wait_receive(
        &mut self,
        py: Python<'_>,
        timeout_seconds: f64,
    ) -> PyResult<Option<PyReceivedPointCloudSample>> {
        ensure_activation_active(self.guard.as_ref())?;
        let timeout = checked_timeout(timeout_seconds)?;
        let (process_instance, activation) = if let Some(guard) = &self.guard {
            (guard.process_instance(), guard.activation())
        } else {
            let activation = ActivationId::new(self.next_activation);
            self.next_activation = self.next_activation.saturating_add(1);
            (self.process_instance, activation)
        };
        let subscriber_cell = Arc::clone(&self.subscriber);
        let guard = self.guard.clone();
        let outcome = py
            .detach(move || {
                let endpoint = subscriber_cell.lock().map_err(|_| {
                    ReceiveError::Wait(NativeWaitError::ListenerWait(
                        "PointCloud subscriber endpoint lock was poisoned".to_owned(),
                    ))
                })?;
                let subscriber = endpoint.as_ref().ok_or_else(|| {
                    ReceiveError::Wait(NativeWaitError::ListenerWait(
                        "PointCloud subscriber endpoint is closed".to_owned(),
                    ))
                })?;
                wait_for_slice(
                    subscriber,
                    process_instance,
                    activation,
                    timeout,
                    guard.as_ref(),
                )
            })
            .map_err(receive_py_error)?;
        self.wait_stats.add(outcome.stats);
        py.check_signals()?;
        Ok(outcome.sample.map(|sample| PyReceivedPointCloudSample {
            sample: Mutex::new(sample),
        }))
    }

    fn pressure<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        endpoint_pressure(
            py,
            &self.subscriber,
            "PointCloud subscriber",
            |subscriber| subscriber.pressure(),
        )
    }

    fn wait_stats(&self) -> (u64, u64, u64, u64, u64) {
        let stats = self.wait_stats;
        (
            stats.waits,
            stats.wakeups,
            stats.empty_wakeups,
            stats.cancellation_wakeups,
            stats.samples_processed,
        )
    }

    fn activation_cancelled(&self) -> bool {
        self.guard.as_ref().is_some_and(|guard| guard.is_expired())
    }

    fn async_receive_supported(&self) -> bool {
        self.async_state.is_some()
    }

    fn _debug_async_wait_stats(&self) -> (u64, u64, u64) {
        self.async_state
            .as_ref()
            .map(|state| state.debug_stats())
            .unwrap_or((0, 0, 0))
    }

    #[pyo3(signature = (loop_object, future))]
    fn begin_receive_async(
        &self,
        loop_object: Bound<'_, PyAny>,
        future: Bound<'_, PyAny>,
    ) -> PyResult<u64> {
        ensure_activation_active(self.guard.as_ref())?;
        let state = self.async_state.as_ref().ok_or_else(|| {
            AsyncRuntimeClosedError::new_err(
                "receive_async is available only for activation-bound subscribers",
            )
        })?;
        state.begin(loop_object, future)
    }

    fn finish_receive_async(&self, generation: u64) -> PyResult<()> {
        let Some(state) = self.async_state.as_ref() else {
            return Err(AsyncRuntimeClosedError::new_err(
                "receive_async is available only for activation-bound subscribers",
            ));
        };
        if let Some(error) = state.finish(generation) {
            return Err(async_runtime::bridge_failure(error));
        }
        Ok(())
    }

    fn cancel_receive_async(&self, generation: u64) {
        if let Some(state) = self.async_state.as_ref() {
            state.cancel(generation);
        }
    }
}

impl PyPointCloudSubscriber {
    pub(crate) fn from_activation(
        subscriber: PointCloudSubscriberCell,
        guard: Arc<ActivationGuard>,
        async_state: Arc<AsyncSubscriberState>,
    ) -> Self {
        let process_instance = guard.process_instance();
        let async_bridge = guard.async_bridge();
        Self {
            subscriber,
            process_instance,
            next_activation: 1,
            wait_stats: CumulativeWaitStats::default(),
            guard: Some(guard),
            async_state: Some(async_state),
            _async_bridge: Some(async_bridge),
        }
    }
}

fn checked_timeout(seconds: f64) -> PyResult<Duration> {
    if !seconds.is_finite() || seconds <= 0.0 {
        return Err(PyValueError::new_err(
            "WaitSet timeout quantum must be finite and greater than zero",
        ));
    }
    Duration::try_from_secs_f64(seconds)
        .map_err(|error| PyValueError::new_err(format!("invalid timeout: {error}")))
}

#[pyfunction]
fn _image_descriptor_json() -> PyResult<String> {
    serde_json::to_string(&rbt_types::Image::descriptor())
        .map_err(|error| RbtError::new_err(error.to_string()))
}

#[pyfunction]
fn _pointcloud_descriptor_json() -> PyResult<String> {
    serde_json::to_string(&rbt_types::PointCloud::descriptor())
        .map_err(|error| RbtError::new_err(error.to_string()))
}

#[pyfunction]
fn _stamped_bytes_descriptor_json() -> PyResult<String> {
    serde_json::to_string(&StampedBytes::descriptor())
        .map_err(|error| RbtError::new_err(error.to_string()))
}

#[pymodule]
fn _rbt(py: Python<'_>, module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add("RbtError", py.get_type::<RbtError>())?;
    module.add("AbiMismatchError", py.get_type::<AbiMismatchError>())?;
    module.add(
        "BorrowLimitReachedError",
        py.get_type::<BorrowLimitReachedError>(),
    )?;
    module.add(
        "InvalidResourceConfigError",
        py.get_type::<InvalidResourceConfigError>(),
    )?;
    module.add(
        "IncompatibleResourceConfigError",
        py.get_type::<IncompatibleResourceConfigError>(),
    )?;
    module.add("WaitError", py.get_type::<WaitError>())?;
    module.add(
        "ConcurrentReceiveError",
        py.get_type::<ConcurrentReceiveError>(),
    )?;
    module.add(
        "AsyncRuntimeClosedError",
        py.get_type::<AsyncRuntimeClosedError>(),
    )?;
    module.add("RpcError", py.get_type::<RpcError>())?;
    module.add("RpcUnavailableError", py.get_type::<RpcUnavailableError>())?;
    module.add(
        "RpcDeadlineExceededError",
        py.get_type::<RpcDeadlineExceededError>(),
    )?;
    module.add("RpcCancelledError", py.get_type::<RpcCancelledError>())?;
    module.add("RpcCapacityError", py.get_type::<RpcCapacityError>())?;
    module.add(
        "RpcActivationExpiredError",
        py.get_type::<RpcActivationExpiredError>(),
    )?;
    module.add("RpcTransportError", py.get_type::<RpcTransportError>())?;
    module.add("InvalidPayloadError", py.get_type::<InvalidPayloadError>())?;
    module.add(
        "InvalidImageLayoutError",
        py.get_type::<InvalidImageLayoutError>(),
    )?;
    module.add(
        "InvalidPayloadAlignmentError",
        py.get_type::<InvalidPayloadAlignmentError>(),
    )?;
    module.add(
        "UnsupportedPixelFormatError",
        py.get_type::<UnsupportedPixelFormatError>(),
    )?;
    module.add(
        "MissingTransportFeatureError",
        py.get_type::<MissingTransportFeatureError>(),
    )?;
    module.add(
        "LoanPoolExhaustedError",
        py.get_type::<LoanPoolExhaustedError>(),
    )?;
    module.add(
        "LoanStillMappedError",
        py.get_type::<LoanStillMappedError>(),
    )?;
    module.add("LoanConsumedError", py.get_type::<LoanConsumedError>())?;
    module.add("QueueFullError", py.get_type::<QueueFullError>())?;
    module.add(
        "UnableToDeliverError",
        py.get_type::<UnableToDeliverError>(),
    )?;
    module.add(
        "ReadinessNotifyError",
        py.get_type::<ReadinessNotifyError>(),
    )?;
    module.add_class::<PyImagePublisher>()?;
    module.add_class::<PyStampedBytesPublisher>()?;
    module.add_class::<PyPointCloudPublisher>()?;
    module.add_class::<PyLoanedImageSample>()?;
    module.add_class::<PyStampedBytesLoan>()?;
    module.add_class::<PyImageLease>()?;
    module.add_class::<PyLoanedPointCloudSample>()?;
    module.add_class::<PyPointCloudLease>()?;
    module.add_class::<PyReceivedImageSample>()?;
    module.add_class::<PyReceivedPointCloudSample>()?;
    module.add_class::<PyImageSubscriber>()?;
    module.add_class::<PyStampedBytesSubscriber>()?;
    module.add_class::<PyPointCloudSubscriber>()?;
    module.add_function(wrap_pyfunction!(_image_descriptor_json, module)?)?;
    module.add_function(wrap_pyfunction!(_pointcloud_descriptor_json, module)?)?;
    module.add_function(wrap_pyfunction!(_stamped_bytes_descriptor_json, module)?)?;
    rpc::add_to_module(py, module)?;
    tf::add_to_module(py, module)?;
    tf_transport::add_to_module(py, module)?;
    component_runtime::add_to_module(py, module)?;
    module.add("__version__", env!("CARGO_PKG_VERSION"))?;
    Ok(())
}

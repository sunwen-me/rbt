//! Native support for the Python asyncio adapter.
//!
//! The component runtime owns one lazily-created CPython event loop.  The
//! loop is deliberately separate from the Tokio lifecycle runtime: Tokio
//! continues to own lifecycle supervision while this thread owns Python
//! coroutine execution.  Stream readiness is bridged by one native
//! [`WaitContext`] per activation; the bridge never receives a payload.

use std::{
    ffi::CStr,
    sync::{
        Arc, Condvar, Mutex, MutexGuard, Weak,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    thread::{self, JoinHandle, ThreadId},
    time::{SystemTime, UNIX_EPOCH},
};

use iceoryx2::{
    node::{Node, NodeBuilder},
    port::listener::Listener,
    prelude::{SignalHandlingMode, ipc_threadsafe},
    service::{builder::event::EventOpenError, service_name::ServiceName},
};
use pyo3::{
    Bound, Py, PyAny, PyErr, PyResult, Python,
    types::{
        PyAnyMethods, PyCFunction, PyDict, PyStringMethods, PyTracebackMethods, PyTuple,
        PyTupleMethods, PyTypeMethods,
    },
};
use rbt_core::{ActivationId, ProcessInstanceId, StreamDescriptor};
use rbt_iox2::{
    EventSource, WaitContext, WaitError as NativeWaitError, WaitExit, WaitProgress, WaitToken,
};
use tokio::{sync::oneshot, task::JoinError};

use crate::{AsyncRuntimeClosedError, WaitError};

const READINESS_SERVICE_SUFFIX: &str = "/ready";

/// A bounded diagnostic produced by a Python asyncio task.
#[derive(Clone, Debug)]
pub(crate) struct AsyncTaskDiagnostic {
    pub(crate) class: String,
    pub(crate) message: String,
    pub(crate) traceback: String,
}

/// Completion of one Python asyncio task.
#[derive(Debug)]
pub(crate) enum AsyncTaskOutcome {
    Completed,
    Error(AsyncTaskDiagnostic),
    Cancelled,
}

/// A process-lifetime managed CPython event loop.
///
/// The loop thread is started on the first async task submission.  Keeping
/// the loop object behind a mutex lets callers submit thread-safe callbacks
/// without ever borrowing Python values across a GIL release.
pub(crate) struct AsyncioRuntime {
    state: Mutex<AsyncioRuntimeState>,
    ready: Condvar,
    thread: Mutex<Option<JoinHandle<()>>>,
}

struct AsyncioRuntimeState {
    started: bool,
    closed: bool,
    loop_object: Option<Arc<Py<PyAny>>>,
    startup_error: Option<String>,
    thread_id: Option<ThreadId>,
}

impl AsyncioRuntime {
    pub(crate) fn new() -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(AsyncioRuntimeState {
                started: false,
                closed: false,
                loop_object: None,
                startup_error: None,
                thread_id: None,
            }),
            ready: Condvar::new(),
            thread: Mutex::new(None),
        })
    }

    fn lock_state(&self) -> Result<MutexGuard<'_, AsyncioRuntimeState>, String> {
        self.state
            .lock()
            .map_err(|_| "managed asyncio runtime state lock was poisoned".to_owned())
    }

    /// Starts the loop exactly once and waits until its Python loop object is
    /// ready for thread-safe submissions.
    pub(crate) fn ensure_started(self: &Arc<Self>) -> PyResult<()> {
        let spawn = {
            let mut state = self
                .lock_state()
                .map_err(AsyncRuntimeClosedError::new_err)?;
            if state.closed {
                return Err(AsyncRuntimeClosedError::new_err(
                    "managed asyncio runtime is closed",
                ));
            }
            if state.started {
                false
            } else {
                state.started = true;
                true
            }
        };

        if spawn {
            let runtime = Arc::clone(self);
            let thread = thread::Builder::new()
                .name("rbt-python-asyncio".to_owned())
                .spawn({
                    let thread_runtime = Arc::clone(&runtime);
                    move || thread_runtime.run_loop()
                })
                .map_err(|error| {
                    if let Ok(mut state) = runtime.state.lock() {
                        state.startup_error = Some(error.to_string());
                        state.closed = true;
                    }
                    runtime.ready.notify_all();
                    AsyncRuntimeClosedError::new_err(format!(
                        "failed to start managed asyncio loop: {error}"
                    ))
                })?;
            if let Ok(mut handle) = self.thread.lock() {
                *handle = Some(thread);
            } else {
                // The loop is still stopped by `shutdown`; losing the join
                // handle is reported as a closed runtime rather than
                // panicking during task submission.
                return Err(AsyncRuntimeClosedError::new_err(
                    "managed asyncio runtime thread lock was poisoned",
                ));
            }
        }

        let mut state = self
            .lock_state()
            .map_err(AsyncRuntimeClosedError::new_err)?;
        while state.loop_object.is_none() && state.startup_error.is_none() && !state.closed {
            state = self.ready.wait(state).map_err(|_| {
                AsyncRuntimeClosedError::new_err(
                    "managed asyncio runtime startup wait was poisoned",
                )
            })?;
        }
        if let Some(error) = &state.startup_error {
            return Err(AsyncRuntimeClosedError::new_err(format!(
                "failed to initialize managed asyncio loop: {error}"
            )));
        }
        if state.loop_object.is_none() || state.closed {
            return Err(AsyncRuntimeClosedError::new_err(
                "managed asyncio runtime is closed",
            ));
        }
        Ok(())
    }

    fn run_loop(self: Arc<Self>) {
        let loop_result = Python::attach(|py| -> PyResult<Py<PyAny>> {
            let asyncio = py.import("asyncio")?;
            let loop_object = asyncio.call_method0("new_event_loop")?;
            asyncio.call_method1("set_event_loop", (loop_object.clone(),))?;
            Ok(loop_object.unbind())
        });

        let loop_object = match loop_result {
            Ok(loop_object) => loop_object,
            Err(error) => {
                let message = error.to_string();
                if let Ok(mut state) = self.state.lock() {
                    state.startup_error = Some(message);
                    state.closed = true;
                    state.thread_id = Some(thread::current().id());
                }
                self.ready.notify_all();
                return;
            }
        };

        let loop_reference = Python::attach(|py| Arc::new(loop_object.clone_ref(py)));
        let should_run = self
            .state
            .lock()
            .map(|state| !state.closed)
            .unwrap_or(false);
        if let Ok(mut state) = self.state.lock() {
            state.loop_object = Some(loop_reference);
            state.thread_id = Some(thread::current().id());
        }
        self.ready.notify_all();

        Python::attach(|py| {
            if should_run {
                let _ = loop_object.bind(py).call_method0("run_forever");
            }

            let loop_object = loop_object.bind(py);
            let _ = loop_object.call_method0("stop");
            let _ = loop_object.call_method0("close");
        });
        if let Ok(mut state) = self.state.lock() {
            state.loop_object = None;
            state.closed = true;
        }
        self.ready.notify_all();
    }

    fn loop_object(&self) -> PyResult<Arc<Py<PyAny>>> {
        let state = self
            .lock_state()
            .map_err(AsyncRuntimeClosedError::new_err)?;
        if state.closed {
            return Err(AsyncRuntimeClosedError::new_err(
                "managed asyncio runtime is closed",
            ));
        }
        state
            .loop_object
            .as_ref()
            .map(Arc::clone)
            .ok_or_else(|| AsyncRuntimeClosedError::new_err("managed asyncio loop is not ready"))
    }

    /// Schedules a Python callback on the managed loop thread.
    pub(crate) fn call_soon_threadsafe<F>(self: &Arc<Self>, callback: F) -> PyResult<()>
    where
        F: FnOnce(Python<'_>) -> PyResult<Bound<'_, PyAny>> + Send + 'static,
    {
        self.ensure_started()?;
        let loop_object = self.loop_object()?;
        Python::attach(|py| {
            let callback = callback(py)?;
            loop_object
                .bind(py)
                .call_method1("call_soon_threadsafe", (callback,))?;
            Ok(())
        })
    }

    /// Schedules creation and execution of a fresh coroutine instance on the
    /// managed loop.  The callable and task context are only invoked on that
    /// loop thread; no coroutine object is retained across activations.
    pub(crate) fn spawn_coroutine(
        self: &Arc<Self>,
        callable: Py<PyAny>,
        task_context: Py<PyAny>,
        control: Arc<AsyncTaskControl>,
        completion: oneshot::Sender<AsyncTaskOutcome>,
    ) -> PyResult<()> {
        self.ensure_started()?;
        let completion = Arc::new(Mutex::new(Some(completion)));
        let loop_object = self.loop_object()?;
        self.call_soon_threadsafe(move |py| {
            let callback = PyCFunction::new_closure(
                py,
                None::<&CStr>,
                None::<&CStr>,
                move |args: &Bound<'_, PyTuple>,
                      _kwargs: Option<&Bound<'_, PyDict>>|
                      -> PyResult<Py<PyAny>> {
                    let py = args.py();
                    let coroutine = match callable.bind(py).call1((task_context.bind(py),)) {
                        Ok(coroutine) => coroutine,
                        Err(error) => {
                            send_outcome(
                                &completion,
                                AsyncTaskOutcome::Error(task_diagnostic(py, error)),
                            );
                            control.mark_done();
                            return Ok(py.None());
                        }
                    };
                    let task = match loop_object
                        .bind(py)
                        .call_method1("create_task", (coroutine,))
                    {
                        Ok(task) => task,
                        Err(error) => {
                            send_outcome(
                                &completion,
                                AsyncTaskOutcome::Error(task_diagnostic(py, error)),
                            );
                            control.mark_done();
                            return Ok(py.None());
                        }
                    };
                    control.set_task(task.clone().unbind());
                    let control_for_callback = Arc::clone(&control);
                    let completion_for_callback = Arc::clone(&completion);
                    let done_callback = match PyCFunction::new_closure(
                        py,
                        None::<&CStr>,
                        None::<&CStr>,
                        move |args: &Bound<'_, PyTuple>,
                              _kwargs: Option<&Bound<'_, PyDict>>|
                              -> PyResult<Py<PyAny>> {
                            let py = args.py();
                            let task = args.get_item(0)?;
                            let outcome = task_outcome(py, task, &control_for_callback);
                            control_for_callback.clear_task();
                            control_for_callback.mark_done();
                            send_outcome(&completion_for_callback, outcome);
                            Ok(py.None())
                        },
                    ) {
                        Ok(callback) => callback,
                        Err(error) => {
                            let _ = task.call_method0("cancel");
                            control.clear_task();
                            control.mark_done();
                            send_outcome(
                                &completion,
                                AsyncTaskOutcome::Error(task_diagnostic(py, error)),
                            );
                            return Ok(py.None());
                        }
                    };
                    if let Err(error) = task.call_method1("add_done_callback", (done_callback,)) {
                        let _ = task.call_method0("cancel");
                        control.clear_task();
                        control.mark_done();
                        send_outcome(
                            &completion,
                            AsyncTaskOutcome::Error(task_diagnostic(py, error)),
                        );
                    }
                    Ok(py.None())
                },
            )?;
            Ok(callback.into_any())
        })
    }

    /// Requests loop shutdown and joins its single thread.  It is safe to
    /// call this more than once and safe when the loop was never started.
    pub(crate) fn shutdown(&self) {
        let (loop_object, thread_id) = {
            let Ok(mut state) = self.state.lock() else {
                return;
            };
            if state.closed {
                (None, state.thread_id)
            } else {
                state.closed = true;
                let loop_object = state.loop_object.as_ref().map(Arc::clone);
                (loop_object, state.thread_id)
            }
        };
        self.ready.notify_all();

        if let Some(loop_object) = loop_object {
            Python::attach(|py| {
                let loop_object = loop_object.bind(py);
                // `loop.stop` itself is not thread-safe; arrange it through
                // call_soon_threadsafe using the loop's bound method.
                let stop = loop_object.getattr("stop");
                if let Ok(stop) = stop {
                    let _ = loop_object.call_method1("call_soon_threadsafe", (stop,));
                }
            });
        }

        if thread_id != Some(thread::current().id())
            && let Ok(mut thread) = self.thread.lock()
            && let Some(thread) = thread.take()
        {
            let _ = thread.join();
        }
    }
}

impl Drop for AsyncioRuntime {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn send_outcome(
    sender: &Arc<Mutex<Option<oneshot::Sender<AsyncTaskOutcome>>>>,
    outcome: AsyncTaskOutcome,
) {
    if let Ok(mut sender) = sender.lock()
        && let Some(sender) = sender.take()
    {
        let _ = sender.send(outcome);
    }
}

fn bounded(mut value: String) -> String {
    const MAX_DIAGNOSTIC_BYTES: usize = 16 * 1024;
    if value.len() > MAX_DIAGNOSTIC_BYTES {
        value.truncate(MAX_DIAGNOSTIC_BYTES.saturating_sub("…".len()));
        value.push('…');
    }
    value
}

fn task_diagnostic(py: Python<'_>, error: PyErr) -> AsyncTaskDiagnostic {
    let class = error
        .get_type(py)
        .name()
        .and_then(|name| name.to_str().map(str::to_owned))
        .unwrap_or_else(|_| "BaseException".to_owned());
    let message = error
        .value(py)
        .str()
        .and_then(|message| message.to_str().map(str::to_owned))
        .unwrap_or_else(|_| "<unprintable Python exception>".to_owned());
    let traceback = error
        .traceback(py)
        .and_then(|traceback| traceback.format().ok())
        .unwrap_or_default();
    AsyncTaskDiagnostic {
        class: bounded(class),
        message: bounded(message),
        traceback: bounded(traceback),
    }
}

fn task_outcome(
    py: Python<'_>,
    task: Bound<'_, PyAny>,
    control: &AsyncTaskControl,
) -> AsyncTaskOutcome {
    let cancelled = task
        .call_method0("cancelled")
        .and_then(|cancelled| cancelled.extract::<bool>())
        .unwrap_or(false);
    if cancelled {
        return if control.activation_cancelled() {
            AsyncTaskOutcome::Cancelled
        } else {
            AsyncTaskOutcome::Error(AsyncTaskDiagnostic {
                class: "asyncio.CancelledError".to_owned(),
                message: "async task was cancelled".to_owned(),
                traceback: String::new(),
            })
        };
    }
    match task.call_method0("result") {
        Ok(result) => {
            drop(result);
            AsyncTaskOutcome::Completed
        }
        Err(error) if control.activation_cancelled() && is_cancelled_error(py, &error) => {
            AsyncTaskOutcome::Cancelled
        }
        Err(error) => {
            let original = error
                .value(py)
                .getattr("_rbt_original")
                .ok()
                .map(PyErr::from_value);
            AsyncTaskOutcome::Error(task_diagnostic(py, original.unwrap_or(error)))
        }
    }
}

fn is_cancelled_error(py: Python<'_>, error: &PyErr) -> bool {
    error
        .get_type(py)
        .name()
        .map(|name| name.to_string_lossy() == "CancelledError")
        .unwrap_or(false)
}

/// Control shared by a staged async task handle and its supervisor wrapper.
pub(crate) struct AsyncTaskControl {
    runtime: Weak<AsyncioRuntime>,
    cancelled: AtomicBool,
    activation_cancelled: AtomicBool,
    done: AtomicBool,
    task: Mutex<Option<Arc<Py<PyAny>>>>,
}

impl AsyncTaskControl {
    pub(crate) fn new(runtime: &Arc<AsyncioRuntime>) -> Arc<Self> {
        Arc::new(Self {
            runtime: Arc::downgrade(runtime),
            cancelled: AtomicBool::new(false),
            activation_cancelled: AtomicBool::new(false),
            done: AtomicBool::new(false),
            task: Mutex::new(None),
        })
    }

    pub(crate) fn cancel(&self) {
        if self.is_done() {
            return;
        }
        self.cancelled.store(true, Ordering::Release);
        self.cancel_python_task();
    }

    pub(crate) fn cancel_for_activation(&self) {
        self.activation_cancelled.store(true, Ordering::Release);
        self.cancel_python_task();
    }

    pub(crate) fn activation_cancelled(&self) -> bool {
        self.activation_cancelled.load(Ordering::Acquire)
    }

    pub(crate) fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }

    pub(crate) fn is_done(&self) -> bool {
        self.done.load(Ordering::Acquire)
    }

    pub(crate) fn mark_done(&self) {
        self.done.store(true, Ordering::Release);
    }

    fn set_task(&self, task: Py<PyAny>) {
        let cancelled = self.is_cancelled() || self.activation_cancelled();
        if let Ok(mut current) = self.task.lock() {
            *current = Some(Arc::new(task));
        }
        if cancelled {
            self.cancel_python_task();
        }
    }

    fn clear_task(&self) {
        if let Ok(mut task) = self.task.lock() {
            task.take();
        }
    }

    fn cancel_python_task(&self) {
        let task = self
            .task
            .lock()
            .ok()
            .and_then(|task| task.as_ref().map(Arc::clone));
        let Some(task) = task else {
            return;
        };
        let Some(runtime) = self.runtime.upgrade() else {
            return;
        };
        let _ = runtime.call_soon_threadsafe(move |py| {
            let callback = PyCFunction::new_closure(
                py,
                None::<&CStr>,
                None::<&CStr>,
                move |args: &Bound<'_, PyTuple>,
                      _kwargs: Option<&Bound<'_, PyDict>>|
                      -> PyResult<Py<PyAny>> {
                    let py = args.py();
                    let task = task.bind(py);
                    if !task
                        .call_method0("done")
                        .and_then(|done| done.extract::<bool>())
                        .unwrap_or(true)
                    {
                        let _ = task.call_method0("cancel");
                    }
                    Ok(py.None())
                },
            )?;
            Ok(callback.into_any())
        });
    }
}

/// A sidecar readiness listener owned by one activation bridge registration.
struct AsyncBridgeListener {
    listener: Listener<ipc_threadsafe::Service>,
    _node: Node<ipc_threadsafe::Service>,
}

impl AsyncBridgeListener {
    fn open(descriptor: &StreamDescriptor) -> Result<Arc<Self>, String> {
        let node = NodeBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<ipc_threadsafe::Service>()
            .map_err(|error| error.to_string())?;
        let name = format!("{}{READINESS_SERVICE_SUFFIX}", descriptor.service_name());
        let service_name: ServiceName = name.as_str().try_into().map_err(
            |error: iceoryx2::service::service_name::ServiceNameError| error.to_string(),
        )?;
        let event = node
            .service_builder(&service_name)
            .event()
            .open()
            .map_err(|error: EventOpenError| error.to_string())?;
        let listener = event
            .listener_builder()
            .create()
            .map_err(|error| error.to_string())?;
        Ok(Arc::new(Self {
            listener,
            _node: node,
        }))
    }

    fn event_listener(&self) -> &Listener<ipc_threadsafe::Service> {
        &self.listener
    }
}

struct PendingReceive {
    generation: u64,
    loop_object: Arc<Py<PyAny>>,
    future: Arc<Py<PyAny>>,
    notification_scheduled: bool,
}

/// Per-subscriber control state.  It stores at most one pending waiter and a
/// single coalesced readiness bit; no payload is retained here.
pub(crate) struct AsyncSubscriberState {
    descriptor: StreamDescriptor,
    listener: Mutex<Option<Arc<AsyncBridgeListener>>>,
    pending: Mutex<Option<PendingReceive>>,
    ready: AtomicBool,
    next_generation: AtomicU64,
    readiness_events: AtomicU64,
    readiness_callbacks: AtomicU64,
    last_readiness_unix_ns: AtomicU64,
    failure: Mutex<Option<String>>,
}

impl AsyncSubscriberState {
    pub(crate) fn new(descriptor: &StreamDescriptor) -> Result<Arc<Self>, String> {
        Ok(Arc::new(Self {
            descriptor: descriptor.clone(),
            listener: Mutex::new(None),
            pending: Mutex::new(None),
            ready: AtomicBool::new(false),
            next_generation: AtomicU64::new(1),
            readiness_events: AtomicU64::new(0),
            readiness_callbacks: AtomicU64::new(0),
            last_readiness_unix_ns: AtomicU64::new(0),
            failure: Mutex::new(None),
        }))
    }

    fn lock_pending(&self) -> Result<MutexGuard<'_, Option<PendingReceive>>, String> {
        self.pending
            .lock()
            .map_err(|_| "async receive state lock was poisoned".to_owned())
    }

    /// Opens this subscriber's sidecar listener once, on the activation
    /// bridge thread.  Construction is intentionally listener-free so a
    /// sync-only activation never creates an extra node or listener.
    fn initialize_listener(&self) -> Result<Arc<AsyncBridgeListener>, String> {
        let existing = self
            .listener
            .lock()
            .map_err(|_| "async readiness listener lock was poisoned".to_owned())?
            .clone();
        if let Some(existing) = existing {
            return Ok(existing);
        }

        let listener = AsyncBridgeListener::open(&self.descriptor)?;
        let mut current = self
            .listener
            .lock()
            .map_err(|_| "async readiness listener lock was poisoned".to_owned())?;
        if let Some(existing) = current.as_ref() {
            Ok(Arc::clone(existing))
        } else {
            *current = Some(Arc::clone(&listener));
            Ok(listener)
        }
    }

    /// Registers one future and returns its monotonic wait generation.
    pub(crate) fn begin(
        self: &Arc<Self>,
        loop_object: Bound<'_, PyAny>,
        future: Bound<'_, PyAny>,
    ) -> PyResult<u64> {
        // Convert the Python borrows before taking the Rust state lock.  The
        // bridge may schedule a callback from another thread; keeping all
        // Python work outside the mutex avoids a GIL/mutex lock inversion.
        let loop_object = Arc::new(loop_object.unbind());
        let future = Arc::new(future.unbind());
        let mut pending = self.lock_pending().map_err(crate::RbtError::new_err)?;
        if pending.is_some() {
            return Err(crate::ConcurrentReceiveError::new_err(
                "only one receive_async waiter may be pending per subscriber",
            ));
        }
        let generation = self
            .next_generation
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |next| {
                next.checked_add(1)
            })
            .map_err(|_| {
                crate::ConcurrentReceiveError::new_err("receive_async generation exhausted")
            })?;
        let mut wait = PendingReceive {
            generation,
            loop_object,
            future,
            notification_scheduled: false,
        };
        let notify_now = self.ready.swap(false, Ordering::AcqRel);
        if notify_now {
            wait.notification_scheduled = true;
        }
        let callback = if notify_now {
            Some((
                wait.generation,
                Arc::clone(&wait.loop_object),
                Arc::clone(&wait.future),
            ))
        } else {
            None
        };
        *pending = Some(wait);
        drop(pending);
        if let Some((generation, loop_object, future)) = callback {
            self.readiness_callbacks.fetch_add(1, Ordering::Relaxed);
            schedule_future(loop_object, future, Arc::downgrade(self), generation);
        }
        // A bridge/listener failure may race with registration.  Re-check it
        // after publishing the waiter so the failure path cannot leave a
        // future asleep between the two mutex acquisitions.
        if self
            .failure
            .lock()
            .map(|failure| failure.is_some())
            .unwrap_or(true)
        {
            self.wake_cancelled();
        }
        Ok(generation)
    }

    /// Coalesces one source-ready event and wakes the current future at most
    /// once.  The callback checks generation and future done state again on
    /// the Python loop thread.
    pub(crate) fn mark_ready(self: &Arc<Self>) {
        self.readiness_events.fetch_add(1, Ordering::Relaxed);
        let unix_ns = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .ok()
            .and_then(|duration| u64::try_from(duration.as_nanos()).ok())
            .unwrap_or(u64::MAX);
        self.last_readiness_unix_ns
            .store(unix_ns, Ordering::Release);
        self.ready.store(true, Ordering::Release);
        let callback = self.lock_pending().ok().and_then(|mut pending| {
            let wait = pending.as_mut()?;
            if wait.notification_scheduled {
                return None;
            }
            wait.notification_scheduled = true;
            Some((
                wait.generation,
                Arc::clone(&wait.loop_object),
                Arc::clone(&wait.future),
            ))
        });
        if let Some((generation, loop_object, future)) = callback {
            self.readiness_callbacks.fetch_add(1, Ordering::Relaxed);
            schedule_future(loop_object, future, Arc::downgrade(self), generation);
        }
    }

    pub(crate) fn debug_stats(&self) -> (u64, u64, u64) {
        (
            self.readiness_events.load(Ordering::Relaxed),
            self.readiness_callbacks.load(Ordering::Relaxed),
            self.last_readiness_unix_ns.load(Ordering::Acquire),
        )
    }

    /// Wakes a pending waiter because activation cancellation was requested.
    pub(crate) fn wake_cancelled(self: &Arc<Self>) {
        let callback = self.lock_pending().ok().and_then(|mut pending| {
            let wait = pending.as_mut()?;
            if wait.notification_scheduled {
                return None;
            }
            wait.notification_scheduled = true;
            Some((
                wait.generation,
                Arc::clone(&wait.loop_object),
                Arc::clone(&wait.future),
            ))
        });
        if let Some((generation, loop_object, future)) = callback {
            schedule_future(loop_object, future, Arc::downgrade(self), generation);
        }
    }

    pub(crate) fn mark_failure(self: &Arc<Self>, message: String) {
        if let Ok(mut failure) = self.failure.lock() {
            *failure = Some(message);
        }
        self.wake_cancelled();
    }

    /// Finishes a generation.  A stale generation is intentionally a no-op;
    /// it cannot remove or complete a replacement waiter.
    pub(crate) fn finish(&self, generation: u64) -> Option<String> {
        {
            let Ok(mut pending) = self.pending.lock() else {
                return Some("async receive state lock was poisoned".to_owned());
            };
            if pending
                .as_ref()
                .is_some_and(|pending| pending.generation == generation)
            {
                pending.take();
                // The completed generation consumed every readiness token
                // coalesced before its callback ran.  Any payload still in
                // the transport is found by the next direct fast-path
                // receive; retaining this bit would schedule a duplicate
                // asyncio callback for the same native event.
                self.ready.store(false, Ordering::Release);
            }
        }
        // Do not hold `pending` while taking `failure`: mark_failure takes
        // those locks in the opposite order (and wakes after releasing both).
        self.failure.lock().ok().and_then(|failure| failure.clone())
    }

    /// Cancels a generation.  A stale generation is intentionally a no-op.
    pub(crate) fn cancel(&self, generation: u64) {
        if let Ok(mut pending) = self.pending.lock()
            && pending
                .as_ref()
                .is_some_and(|pending| pending.generation == generation)
        {
            pending.take();
            // Cancellation never prefetches the sample.  Clear only the
            // control token; a racing/queued payload remains in iceoryx2 and
            // is acquired by a later direct try_receive() fast path.
            self.ready.store(false, Ordering::Release);
        }
    }

    fn complete_notification(&self, generation: u64, future: Bound<'_, PyAny>) {
        let current = self
            .pending
            .lock()
            .ok()
            .and_then(|pending| {
                pending
                    .as_ref()
                    .map(|pending| pending.generation == generation)
            })
            .unwrap_or(false);
        if !current {
            return;
        }
        let done = future
            .call_method0("done")
            .and_then(|done| done.extract::<bool>())
            .unwrap_or(true);
        if !done {
            let py = future.py();
            let _ = future.call_method1("set_result", (py.None(),));
        }
    }
}

fn schedule_future(
    loop_object: Arc<Py<PyAny>>,
    future: Arc<Py<PyAny>>,
    state: Weak<AsyncSubscriberState>,
    generation: u64,
) {
    Python::attach(|py| {
        let callback = PyCFunction::new_closure(
            py,
            None::<&CStr>,
            None::<&CStr>,
            move |_args: &Bound<'_, PyTuple>,
                  _kwargs: Option<&Bound<'_, PyDict>>|
                  -> PyResult<Py<PyAny>> {
                let py = _args.py();
                if let Some(state) = state.upgrade() {
                    state.complete_notification(generation, future.bind(py).clone());
                }
                Ok(py.None())
            },
        );
        if let Ok(callback) = callback {
            let _ = loop_object
                .bind(py)
                .call_method1("call_soon_threadsafe", (callback,));
        }
    });
}

/// One native blocking WaitSet bridge for all activation-bound async
/// subscriber states.
pub(crate) struct AsyncActivationBridge {
    process: ProcessInstanceId,
    activation: ActivationId,
    state: Mutex<BridgeState>,
}

struct BridgeState {
    registrations: Vec<Arc<AsyncSubscriberState>>,
    started: bool,
    stopped: bool,
    wake: Option<rbt_iox2::WaitWakeHandle>,
    thread: Option<JoinHandle<()>>,
}

impl AsyncActivationBridge {
    pub(crate) fn new(process: ProcessInstanceId, activation: ActivationId) -> Arc<Self> {
        Arc::new(Self {
            process,
            activation,
            state: Mutex::new(BridgeState {
                registrations: Vec::new(),
                started: false,
                stopped: false,
                wake: None,
                thread: None,
            }),
        })
    }

    pub(crate) fn register(&self, state: Arc<AsyncSubscriberState>) -> Result<(), String> {
        let mut bridge = self
            .state
            .lock()
            .map_err(|_| "async activation bridge state lock was poisoned".to_owned())?;
        if bridge.started || bridge.stopped {
            return Err("async activation bridge is already sealed".to_owned());
        }
        bridge.registrations.push(state);
        Ok(())
    }

    pub(crate) fn start(self: &Arc<Self>) {
        let registrations = {
            let Ok(mut bridge) = self.state.lock() else {
                return;
            };
            if bridge.started || bridge.stopped || bridge.registrations.is_empty() {
                bridge.started = true;
                return;
            }
            bridge.started = true;
            bridge.registrations.clone()
        };
        let process = self.process;
        let activation = self.activation;
        let bridge = Arc::clone(self);
        let failure_registrations = registrations.clone();
        let thread = thread::Builder::new()
            .name(format!("rbt-wait-bridge-{}", activation.value()))
            .spawn(move || run_bridge(bridge, process, activation, registrations));
        let thread = match thread {
            Ok(thread) => thread,
            Err(error) => {
                for registration in failure_registrations {
                    registration.mark_failure(error.to_string());
                }
                return;
            }
        };
        if let Ok(mut state) = self.state.lock() {
            state.thread = Some(thread);
            if state.stopped
                && let Some(wake) = &state.wake
            {
                let _ = wake.wake();
            }
        }
    }

    pub(crate) fn stop_and_join(&self) {
        let (wake, thread, registrations) = {
            let Ok(mut state) = self.state.lock() else {
                return;
            };
            state.stopped = true;
            let wake = state.wake.clone();
            let thread = if state
                .thread
                .as_ref()
                .is_some_and(|thread| thread.thread().id() == thread::current().id())
            {
                None
            } else {
                state.thread.take()
            };
            (wake, thread, state.registrations.clone())
        };
        for registration in registrations {
            registration.wake_cancelled();
        }
        if let Some(wake) = wake {
            let _ = wake.wake();
        }
        if let Some(thread) = thread {
            let _ = thread.join();
        }
    }
}

impl Drop for AsyncActivationBridge {
    fn drop(&mut self) {
        self.stop_and_join();
    }
}

fn run_bridge(
    bridge: Arc<AsyncActivationBridge>,
    process: ProcessInstanceId,
    activation: ActivationId,
    registrations: Vec<Arc<AsyncSubscriberState>>,
) {
    let listeners = match registrations
        .iter()
        .map(|registration| registration.initialize_listener())
        .collect::<Result<Vec<_>, _>>()
    {
        Ok(listeners) => listeners,
        Err(error) => {
            for registration in &registrations {
                registration.mark_failure(error.clone());
            }
            return;
        }
    };

    let mut context = match WaitContext::new(process, activation) {
        Ok(context) => context,
        Err(error) => {
            for registration in &registrations {
                registration.mark_failure(error.to_string());
            }
            return;
        }
    };
    let wake = context.wake_handle();
    let stopped = bridge
        .state
        .lock()
        .map(|mut state| {
            state.wake = Some(wake.clone());
            state.stopped
        })
        .unwrap_or(true);
    if stopped {
        let _ = wake.wake();
        return;
    }

    let sources = listeners
        .iter()
        .enumerate()
        .map(|(index, listener)| {
            EventSource::new(
                WaitToken::new((index as u64).saturating_add(1)),
                listener.event_listener(),
            )
        })
        .collect::<Vec<_>>();
    let result: Result<WaitExit, NativeWaitError> = context.run(&sources, |event, _budget| {
        if event.is_cancellation() {
            return Ok(WaitProgress::Stop);
        }
        let index = event.token().value().saturating_sub(1) as usize;
        if let Some(registration) = registrations.get(index) {
            registration.mark_ready();
        }
        Ok(WaitProgress::Continue)
    });
    if let Err(error) = result {
        for registration in &registrations {
            registration.mark_failure(error.to_string());
        }
    }
}

/// Converts a native readiness bridge failure into the public Python error.
pub(crate) fn bridge_failure(message: String) -> PyErr {
    WaitError::new_err(message)
}

#[allow(dead_code)]
fn _join_error_message(error: JoinError) -> String {
    if error.is_cancelled() {
        "managed asyncio task join was cancelled".to_owned()
    } else {
        "managed asyncio task panicked".to_owned()
    }
}

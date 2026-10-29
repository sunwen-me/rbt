//! Activation-scoped cleanup and cancellable background tasks.
//!
//! Every activation should own one [`EffectScope`].  Cleanup actions are
//! executed in strict last-in-first-out order by [`EffectScope::shutdown`].
//! Explicit shutdown is required for asynchronous cleanup: `Drop` cannot
//! await futures and therefore only runs synchronous cleanup and cancels or
//! aborts scoped tasks as a last line of defence.

use std::{
    any::Any,
    future::Future,
    pin::Pin,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    task::{Context, Poll, Wake, Waker},
    thread,
    time::Duration,
};

use tokio::task::{AbortHandle, JoinHandle};
use tokio_util::sync::CancellationToken;
use tracing::warn;

type AsyncCleanup = Box<dyn FnOnce() -> Pin<Box<dyn Future<Output = ()> + Send>> + Send + 'static>;
type PreShutdownWake = Box<dyn FnOnce() + Send + 'static>;

enum ScopeAction {
    Sync(Box<dyn FnOnce() + Send + 'static>),
    Async(AsyncCleanup),
    Owned(Box<dyn Any + Send + 'static>),
    Task(TaskAction),
    BlockingTask(BlockingTaskAction),
}

struct TaskAction {
    token: CancellationToken,
    handle: JoinHandle<()>,
}

impl TaskAction {
    fn cancel_and_abort(&self) {
        self.token.cancel();
        self.handle.abort();
    }

    async fn shutdown(mut self) {
        self.token.cancel();
        // Give task bodies a short opportunity to observe the token and run
        // their own async cleanup. A task that ignores cancellation cannot
        // hold activation shutdown open forever: it is aborted after the
        // grace period.
        if tokio::time::timeout(Duration::from_millis(100), &mut self.handle)
            .await
            .is_err()
        {
            self.handle.abort();
            let _ = (&mut self.handle).await;
        }
    }
}

impl Drop for TaskAction {
    fn drop(&mut self) {
        self.cancel_and_abort();
    }
}

type BlockingWake = Arc<dyn Fn() + Send + Sync + 'static>;

struct BlockingTaskControl {
    token: CancellationToken,
    wake: BlockingWake,
    join: Mutex<Option<JoinHandle<()>>>,
    wake_called: AtomicBool,
}

impl BlockingTaskControl {
    fn cancel_and_wake(&self) {
        self.token.cancel();
        if !self.wake_called.swap(true, Ordering::AcqRel) {
            (self.wake)();
        }
    }

    fn take_join(&self) -> Option<JoinHandle<()>> {
        self.join
            .lock()
            .expect("blocking task join mutex poisoned")
            .take()
    }

    async fn join(&self) {
        if let Some(handle) = self.take_join() {
            let _ = handle.await;
        }
    }

    fn cancel_wake_and_join(&self) {
        self.cancel_and_wake();
        if let Some(handle) = self.take_join() {
            join_blocking(handle);
        }
    }
}

struct BlockingTaskAction {
    control: Arc<BlockingTaskControl>,
}

impl BlockingTaskAction {
    async fn shutdown(self) {
        self.control.cancel_and_wake();
        self.control.join().await;
    }

    fn cancel_and_wake(&self) {
        self.control.cancel_and_wake();
    }
}

impl Drop for BlockingTaskAction {
    fn drop(&mut self) {
        // Drop cannot await. The wake is still required so a blocking task
        // does not remain asleep after its activation scope is released.
        self.control.cancel_and_wake();
    }
}

/// A small synchronous parker used when a blocking task is cancelled through
/// [`ScopedTaskHandle::cancel`]. Tokio's blocking task handle is a Future, so
/// polling it here joins the task without trying to abort a running blocking
/// call. This path is only used for blocking tasks; ordinary async tasks keep
/// their existing abort semantics.
struct JoinParker {
    thread: thread::Thread,
    notified: AtomicBool,
}

impl JoinParker {
    fn notify(&self) {
        self.notified.store(true, Ordering::Release);
        self.thread.unpark();
    }

    fn wait(&self) {
        while !self.notified.swap(false, Ordering::Acquire) {
            thread::park();
        }
    }
}

impl Wake for JoinParker {
    fn wake(self: Arc<Self>) {
        self.notify();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.notify();
    }
}

fn join_blocking(handle: JoinHandle<()>) {
    let parker = Arc::new(JoinParker {
        thread: thread::current(),
        notified: AtomicBool::new(false),
    });
    let waker = Waker::from(Arc::clone(&parker));
    let mut context = Context::from_waker(&waker);
    let mut handle = Box::pin(handle);

    loop {
        match Future::poll(handle.as_mut(), &mut context) {
            Poll::Ready(_) => return,
            Poll::Pending => parker.wait(),
        }
    }
}

/// A handle that lets a caller cancel one scoped task before scope shutdown.
#[derive(Clone)]
pub struct ScopedTaskHandle {
    token: CancellationToken,
    abort: Option<AbortHandle>,
    blocking: Option<Arc<BlockingTaskControl>>,
}

impl std::fmt::Debug for ScopedTaskHandle {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("ScopedTaskHandle")
            .field("cancelled", &self.is_cancelled())
            .field("blocking", &self.blocking.is_some())
            .finish()
    }
}

impl ScopedTaskHandle {
    /// Cancels the task cooperatively.
    ///
    /// Blocking tasks are woken and joined before this method returns. Async
    /// tasks retain their existing cancellation and wrapper-abort behavior.
    pub fn cancel(&self) {
        if let Some(blocking) = &self.blocking {
            blocking.cancel_wake_and_join();
        } else {
            self.token.cancel();
            if let Some(abort) = &self.abort {
                abort.abort();
            }
        }
    }

    /// Returns whether cancellation has been requested.
    pub fn is_cancelled(&self) -> bool {
        self.token.is_cancelled()
    }

    /// Returns the task's child cancellation token.
    pub fn cancellation_token(&self) -> CancellationToken {
        self.token.clone()
    }
}

/// Cleanup and task ownership for one component activation.
pub struct EffectScope {
    actions: Vec<ScopeAction>,
    pre_shutdown_wakes: Vec<PreShutdownWake>,
    token: CancellationToken,
    shutdown: bool,
}

impl Default for EffectScope {
    fn default() -> Self {
        Self::new()
    }
}

impl EffectScope {
    /// Creates an empty activation scope.
    pub fn new() -> Self {
        Self {
            actions: Vec::new(),
            pre_shutdown_wakes: Vec::new(),
            token: CancellationToken::new(),
            shutdown: false,
        }
    }

    /// Returns the parent cancellation token for this activation.
    pub fn cancellation_token(&self) -> CancellationToken {
        self.token.clone()
    }

    /// Adds synchronous cleanup to the LIFO stack.
    pub fn defer<F>(&mut self, cleanup: F)
    where
        F: FnOnce() + Send + 'static,
    {
        if self.shutdown {
            cleanup();
        } else {
            self.actions.push(ScopeAction::Sync(Box::new(cleanup)));
        }
    }

    /// Alias emphasizing that the callback is synchronous.
    pub fn defer_sync<F>(&mut self, cleanup: F)
    where
        F: FnOnce() + Send + 'static,
    {
        self.defer(cleanup);
    }

    /// Adds asynchronous cleanup to the LIFO stack.
    ///
    /// The future is run only by explicit [`EffectScope::shutdown`].  Dropping
    /// a scope cannot run this cleanup because `Drop` is synchronous.
    pub fn defer_async<Fut>(&mut self, cleanup: Fut)
    where
        Fut: Future<Output = ()> + Send + 'static,
    {
        if self.shutdown {
            warn!("async cleanup registered after EffectScope shutdown; dropping it");
        } else {
            self.actions
                .push(ScopeAction::Async(Box::new(move || Box::pin(cleanup))));
        }
    }

    /// Adds lazily-created asynchronous cleanup to the LIFO stack.
    ///
    /// This variant is useful when constructing the future itself has side
    /// effects that should happen only during shutdown.  For an already
    /// constructed future, use [`EffectScope::defer_async`].
    pub fn defer_async_with<F, Fut>(&mut self, cleanup: F)
    where
        F: FnOnce() -> Fut + Send + 'static,
        Fut: Future<Output = ()> + Send + 'static,
    {
        if self.shutdown {
            warn!("async cleanup registered after EffectScope shutdown; dropping it");
        } else {
            self.actions
                .push(ScopeAction::Async(Box::new(move || Box::pin(cleanup()))));
        }
    }

    /// Owns an opaque resource until this scope is shut down or dropped.
    ///
    /// Resources participate in the same strict LIFO order as deferred
    /// callbacks.  Their ordinary `Drop` implementation is the cleanup hook.
    pub fn own<T>(&mut self, resource: T)
    where
        T: Send + 'static,
    {
        if self.shutdown {
            drop(resource);
        } else {
            self.actions.push(ScopeAction::Owned(Box::new(resource)));
        }
    }

    /// Alias for [`EffectScope::own`].
    pub fn own_resource<T>(&mut self, resource: T)
    where
        T: Send + 'static,
    {
        self.own(resource);
    }

    /// Registers an activation-expiry wake that runs before scoped tasks are
    /// joined and before ordinary LIFO cleanup begins.
    ///
    /// This is intended for activation-owned blocking endpoints such as RPC
    /// clients and servers. Their waits must observe expiry before teardown
    /// waits for the task that owns the endpoint. Hooks run once in reverse
    /// registration order during either explicit shutdown or `Drop`.
    pub fn wake_on_shutdown<F>(&mut self, wake: F)
    where
        F: FnOnce() + Send + 'static,
    {
        if self.shutdown {
            wake();
        } else {
            self.pre_shutdown_wakes.push(Box::new(wake));
        }
    }

    /// Spawns a task tied to this scope's cancellation lifetime.
    ///
    /// The returned handle is optional from a lifecycle perspective: the
    /// scope always cancels and aborts the task during shutdown.  A caller can
    /// use it for early cancellation when needed.
    pub fn spawn<Fut>(&mut self, future: Fut) -> ScopedTaskHandle
    where
        Fut: Future<Output = ()> + Send + 'static,
    {
        self.spawn_with_token(move |_| future)
    }

    /// Spawns a task after giving it a child cancellation token.
    pub fn spawn_cancellable<F, Fut>(&mut self, task: F) -> ScopedTaskHandle
    where
        F: FnOnce(CancellationToken) -> Fut + Send + 'static,
        Fut: Future<Output = ()> + Send + 'static,
    {
        self.spawn_with_token(task)
    }

    /// Explicitly named variant of [`EffectScope::spawn_cancellable`].
    pub fn spawn_with_token<F, Fut>(&mut self, task: F) -> ScopedTaskHandle
    where
        F: FnOnce(CancellationToken) -> Fut + Send + 'static,
        Fut: Future<Output = ()> + Send + 'static,
    {
        let token = self.token.child_token();
        if self.shutdown {
            token.cancel();
            return ScopedTaskHandle {
                token,
                abort: None,
                blocking: None,
            };
        }

        let task_token = token.clone();
        let handle = tokio::spawn(async move {
            task(task_token).await;
        });
        let abort = handle.abort_handle();
        let task_handle = ScopedTaskHandle {
            token,
            abort: Some(abort),
            blocking: None,
        };
        self.actions.push(ScopeAction::Task(TaskAction {
            token: task_handle.token.clone(),
            handle,
        }));
        task_handle
    }

    /// Spawns a synchronous blocking task tied to this scope's cancellation
    /// lifetime.
    ///
    /// `wake` must make progress on the task's blocking wait (for example by
    /// notifying a condition variable or a synchronous WaitSet event). It is
    /// retained behind a clonable callback so cancellation can wake the task
    /// before its Tokio blocking handle is joined.
    pub fn spawn_blocking_cancellable<F, W, R>(&mut self, task: F, wake: W) -> ScopedTaskHandle
    where
        F: FnOnce(CancellationToken) -> R + Send + 'static,
        W: Fn() + Send + Sync + 'static,
        R: Send + 'static,
    {
        let token = self.token.child_token();
        if self.shutdown {
            token.cancel();
            return ScopedTaskHandle {
                token,
                abort: None,
                blocking: None,
            };
        }

        let control = Arc::new(BlockingTaskControl {
            token: token.clone(),
            wake: Arc::new(wake),
            join: Mutex::new(None),
            wake_called: AtomicBool::new(false),
        });
        let task_token = token.clone();
        let handle = tokio::task::spawn_blocking(move || {
            let _ = task(task_token);
        });
        *control
            .join
            .lock()
            .expect("blocking task join mutex poisoned") = Some(handle);

        self.actions
            .push(ScopeAction::BlockingTask(BlockingTaskAction {
                control: Arc::clone(&control),
            }));
        ScopedTaskHandle {
            token,
            abort: None,
            blocking: Some(control),
        }
    }

    /// Runs every remaining cleanup action in strict LIFO order.
    ///
    /// Calling this method more than once is harmless.  Async cleanup is
    /// guaranteed to run here, which is why activation owners should always
    /// explicitly await this method before releasing their scope.
    pub async fn shutdown(&mut self) {
        if self.shutdown {
            return;
        }
        self.shutdown = true;
        self.run_pre_shutdown_wakes();
        self.token.cancel();
        self.cancel_blocking_tasks();

        while let Some(action) = self.actions.pop() {
            match action {
                ScopeAction::Sync(cleanup) => cleanup(),
                ScopeAction::Async(cleanup) => cleanup().await,
                ScopeAction::Owned(resource) => drop(resource),
                ScopeAction::Task(task) => task.shutdown().await,
                ScopeAction::BlockingTask(task) => task.shutdown().await,
            }
        }
    }

    fn cancel_blocking_tasks(&self) {
        // Request every blocking task's wake before any action (including a
        // resource cleanup registered later in the LIFO stack) can run.
        for action in &self.actions {
            if let ScopeAction::BlockingTask(task) = action {
                task.cancel_and_wake();
            }
        }
    }

    fn run_pre_shutdown_wakes(&mut self) {
        while let Some(wake) = self.pre_shutdown_wakes.pop() {
            wake();
        }
    }
}

impl Drop for EffectScope {
    fn drop(&mut self) {
        if self.shutdown {
            return;
        }

        self.shutdown = true;
        self.run_pre_shutdown_wakes();
        self.token.cancel();
        self.cancel_blocking_tasks();
        while let Some(action) = self.actions.pop() {
            match action {
                ScopeAction::Sync(cleanup) => cleanup(),
                ScopeAction::Owned(resource) => drop(resource),
                ScopeAction::Task(task) => task.cancel_and_abort(),
                ScopeAction::BlockingTask(task) => task.cancel_and_wake(),
                ScopeAction::Async(_) => {
                    warn!(
                        "async EffectScope cleanup was not awaited; call shutdown().await explicitly"
                    );
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use std::{
        sync::{Arc, Condvar, Mutex, mpsc},
        time::Duration,
    };

    use super::*;

    #[tokio::test(flavor = "current_thread")]
    async fn shutdown_runs_mixed_cleanup_in_strict_lifo_order() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut scope = EffectScope::new();
        for label in ["A", "B", "C"] {
            let events = Arc::clone(&events);
            scope.defer(move || events.lock().unwrap().push(label));
        }

        scope.shutdown().await;
        scope.shutdown().await;
        assert_eq!(*events.lock().unwrap(), vec!["C", "B", "A"]);
    }

    #[tokio::test(flavor = "current_thread")]
    async fn scoped_task_stops_during_shutdown() {
        let stopped = Arc::new(tokio::sync::Notify::new());
        let stopped_task = Arc::clone(&stopped);
        let mut scope = EffectScope::new();
        let handle = scope.spawn_cancellable(move |token| async move {
            token.cancelled().await;
            stopped_task.notify_one();
        });

        scope.shutdown().await;
        assert!(handle.is_cancelled());
        tokio::time::timeout(Duration::from_secs(1), stopped.notified())
            .await
            .expect("task must observe cancellation before shutdown returns");
    }

    #[tokio::test(flavor = "current_thread")]
    async fn async_cleanup_runs_when_explicitly_shutdown() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut scope = EffectScope::new();
        let events_for_async = Arc::clone(&events);
        scope.defer_async(async move {
            events_for_async.lock().unwrap().push("async");
        });
        scope.shutdown().await;
        assert_eq!(*events.lock().unwrap(), vec!["async"]);
    }

    #[tokio::test(flavor = "current_thread")]
    async fn blocking_task_is_woken_and_joined_during_shutdown() {
        let wait = Arc::new((Mutex::new(false), Condvar::new()));
        let (started_tx, started_rx) = mpsc::channel();
        let (finished_tx, finished_rx) = mpsc::channel();
        let task_wait = Arc::clone(&wait);
        let wake_wait = Arc::clone(&wait);
        let mut scope = EffectScope::new();
        let handle = scope.spawn_blocking_cancellable(
            move |token| {
                started_tx.send(()).unwrap();
                let (ready, condition) = &*task_wait;
                let mut ready = ready.lock().unwrap();
                while !*ready && !token.is_cancelled() {
                    ready = condition.wait(ready).unwrap();
                }
                finished_tx.send(()).unwrap();
            },
            move || {
                let (ready, condition) = &*wake_wait;
                *ready.lock().unwrap() = true;
                condition.notify_all();
            },
        );

        started_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("blocking task should enter its wait");
        scope.shutdown().await;

        assert!(handle.is_cancelled());
        assert!(
            finished_rx.try_recv().is_ok(),
            "shutdown must join the task"
        );
    }

    #[tokio::test(flavor = "current_thread")]
    async fn cancelling_blocking_handle_wakes_and_joins_task() {
        let wait = Arc::new((Mutex::new(false), Condvar::new()));
        let (started_tx, started_rx) = mpsc::channel();
        let (finished_tx, finished_rx) = mpsc::channel();
        let task_wait = Arc::clone(&wait);
        let wake_wait = Arc::clone(&wait);
        let mut scope = EffectScope::new();
        let handle = scope.spawn_blocking_cancellable(
            move |token| {
                started_tx.send(()).unwrap();
                let (ready, condition) = &*task_wait;
                let mut ready = ready.lock().unwrap();
                while !*ready && !token.is_cancelled() {
                    ready = condition.wait(ready).unwrap();
                }
                finished_tx.send(()).unwrap();
            },
            move || {
                let (ready, condition) = &*wake_wait;
                *ready.lock().unwrap() = true;
                condition.notify_all();
            },
        );

        started_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("blocking task should enter its wait");
        handle.cancel();

        assert!(handle.is_cancelled());
        assert!(finished_rx.try_recv().is_ok(), "cancel must join the task");
        scope.shutdown().await;
    }

    #[tokio::test(flavor = "current_thread")]
    async fn blocking_task_wakes_before_lifo_resource_cleanup() {
        let wait = Arc::new((Mutex::new(false), Condvar::new()));
        let events = Arc::new(Mutex::new(Vec::new()));
        let (started_tx, started_rx) = mpsc::channel();
        let task_wait = Arc::clone(&wait);
        let wake_wait = Arc::clone(&wait);
        let task_events = Arc::clone(&events);
        let wake_events = Arc::clone(&events);
        let cleanup_events = Arc::clone(&events);
        let mut scope = EffectScope::new();
        // Register the resource first so the task's join remains the first
        // LIFO action during shutdown.
        scope.defer(move || cleanup_events.lock().unwrap().push("cleanup"));
        scope.spawn_blocking_cancellable(
            move |token| {
                started_tx.send(()).unwrap();
                let (ready, condition) = &*task_wait;
                let mut ready = ready.lock().unwrap();
                while !*ready && !token.is_cancelled() {
                    ready = condition.wait(ready).unwrap();
                }
                task_events.lock().unwrap().push("finished");
            },
            move || {
                wake_events.lock().unwrap().push("wake");
                let (ready, condition) = &*wake_wait;
                *ready.lock().unwrap() = true;
                condition.notify_all();
            },
        );

        started_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("blocking task should enter its wait");
        scope.shutdown().await;

        assert_eq!(*events.lock().unwrap(), vec!["wake", "finished", "cleanup"]);
    }
}

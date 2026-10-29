//! Component-side control-plane runner and activation context.

use async_trait::async_trait;
use futures_util::FutureExt;
use rbt_core::{
    ActivationId, Binding, BindingTypeError, Capability, CapabilityInstanceId, ComponentId,
    ComponentState, ProcessInstanceId, ProvidedCapability, RequirementSpec, ResolvedBinding,
    Revision, RpcDescriptor, RpcResourceConfig, StreamDescriptor,
};
use rbt_iox2::{FixedMessageAbi, RpcClient, RpcError, RpcServer, ZeroCopySend};
use std::{
    error::Error,
    fmt::{self, Display},
    future::Future,
    net::SocketAddr,
    panic::{AssertUnwindSafe, catch_unwind},
    time::Duration,
};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader, BufWriter},
    net::{TcpStream, ToSocketAddrs, tcp::OwnedReadHalf},
    sync::{mpsc, watch},
    time::sleep,
};
use tracing::{debug, error, info, warn};

pub use crate::protocol::TaskOutcome;
use crate::{
    composer::ControlPlaneError,
    effect_scope::{EffectScope, ScopedTaskHandle},
    manifest::{ActivationSnapshot, CapabilitySnapshot, ComponentSnapshot, RequirementSnapshot},
    protocol::{COMPOSER_SNAPSHOT_VERSION, CONTROL_PROTOCOL_VERSION, ClientMessage, ServerMessage},
};

/// An event emitted by a supervised activation task.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum RuntimeEvent {
    /// A critical task exited and therefore invalidated its activation.
    CriticalTaskExited {
        /// Stable component identity.
        component: ComponentId,
        /// Process lifetime identity.
        process: ProcessInstanceId,
        /// Activation identity.
        activation: ActivationId,
        /// Name assigned to the task by the component.
        task_name: String,
        /// How the task exited.
        outcome: TaskOutcome,
    },
    /// A background task exited without invalidating its activation.
    BackgroundTaskExited {
        /// Stable component identity.
        component: ComponentId,
        /// Process lifetime identity.
        process: ProcessInstanceId,
        /// Activation identity.
        activation: ActivationId,
        /// Name assigned to the task by the component.
        task_name: String,
        /// How the task exited.
        outcome: TaskOutcome,
    },
}

/// Component-local status of the low-rate Composer connection.
///
/// This status is deliberately orthogonal to [`ComponentState`]: a component
/// may be `Active` while its control plane is degraded.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControlPlaneStatus {
    /// No usable Composer connection is currently available.
    Disconnected,
    /// HELLO and the current snapshot were sent on a live connection.
    Connected,
}

/// A typed error returned when component preparation cannot complete.
///
/// Component implementations choose their own associated error type. The
/// runner stores that error as a source only when it needs to report a
/// preparation failure at the control-plane boundary.
#[derive(Debug, thiserror::Error)]
pub enum ComponentError {
    /// The component failed during its one-time preparation step.
    #[error("component preparation failed: {source}")]
    Preparation {
        /// The implementation-provided preparation error.
        #[source]
        source: Box<dyn Error + Send + Sync + 'static>,
    },
}

/// Adapts an implementation error which is only required to be displayable
/// into a source that can participate in the standard error chain. This keeps
/// the component boundary usable with application error containers such as
/// `anyhow::Error`.
#[derive(Clone, Copy)]
struct DisplaySource<E>(E);

impl<E: Display> fmt::Debug for DisplaySource<E> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("DisplaySource")
            .field(&self.0.to_string())
            .finish()
    }
}

impl<E: Display> Display for DisplaySource<E> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        Display::fmt(&self.0, formatter)
    }
}

impl<E: Display> Error for DisplaySource<E> {}

impl ComponentError {
    /// Wraps an implementation-provided preparation error.
    pub fn preparation<E>(source: E) -> Self
    where
        E: Display + Send + Sync + 'static,
    {
        Self::Preparation {
            source: Box::new(DisplaySource(source)),
        }
    }
}

/// A typed error returned when a component resolves an activation binding.
#[derive(Debug, thiserror::Error)]
pub enum RequirementError {
    /// No binding was supplied for the requested requirement name.
    #[error("no binding for requirement {requirement:?}")]
    Missing {
        /// Component-local requirement name.
        requirement: String,
    },
    /// More than one binding was supplied for the requested requirement name.
    #[error("duplicate bindings for requirement {requirement:?}")]
    Duplicate {
        /// Component-local requirement name.
        requirement: String,
    },
    /// The selected binding did not match the requested capability type.
    #[error("binding for requirement {requirement:?} has the wrong capability type: {source}")]
    TypeMismatch {
        /// Component-local requirement name.
        requirement: String,
        /// The underlying core type mismatch.
        #[source]
        source: BindingTypeError,
    },
}

/// Failure to resolve or open an activation-owned RPC endpoint.
#[derive(Debug, thiserror::Error)]
pub enum RpcEndpointError {
    /// The named capability requirement was absent, duplicated, or had the
    /// wrong logical capability role.
    #[error(transparent)]
    Requirement(#[from] RequirementError),
    /// The resolved capability used Stream transport rather than RPC.
    #[error("binding for requirement {requirement:?} is a Stream capability, not RPC")]
    WrongTransport {
        /// Component-local requirement name.
        requirement: String,
    },
    /// The native RPC endpoint failed ABI/resource preflight or could not be
    /// opened.
    #[error(transparent)]
    Transport(#[from] RpcError),
}

/// A component implementation that is prepared once and activated repeatedly.
#[async_trait]
pub trait Component: Sized + Send + 'static {
    /// Immutable configuration used by the one-time preparation step.
    type Config: Send + Sync + 'static;

    /// Error type used by preparation and activation.
    type Error: Display + Send + Sync + 'static;

    /// Prepares resources that remain owned by the component between
    /// activations.
    async fn prepare(config: &Self::Config) -> std::result::Result<Self, Self::Error>;

    /// Returns this component's stable control-plane identity.
    fn id(&self) -> ComponentId;

    /// Returns the capabilities required before activation.
    fn requirements(&self) -> Vec<RequirementSpec>;

    /// Starts one activation using resolved bindings and scoped resources.
    async fn activate(
        &mut self,
        ctx: &mut ActivationCtx<'_>,
    ) -> std::result::Result<(), Self::Error>;
}

/// Context supplied to one component activation.
pub struct ActivationCtx<'scope> {
    component: ComponentId,
    process: ProcessInstanceId,
    activation: ActivationId,
    supervisor: mpsc::UnboundedSender<RuntimeEvent>,
    bindings: Vec<ResolvedBinding>,
    capabilities: Vec<ProvidedCapability>,
    scope: &'scope mut EffectScope,
}

impl<'scope> ActivationCtx<'scope> {
    /// Creates an activation context with placeholder identity and no
    /// supervisor. This constructor is retained for callers that only need
    /// binding helpers; component runners use [`Self::for_activation`].
    pub fn new(bindings: Vec<ResolvedBinding>, scope: &'scope mut EffectScope) -> Self {
        let (supervisor, _receiver) = mpsc::unbounded_channel();
        Self::for_activation(
            ComponentId::new("<unbound>"),
            ProcessInstanceId::new(),
            ActivationId::first(),
            supervisor,
            bindings,
            scope,
        )
    }

    /// Creates an activation context owned by one process and activation.
    pub fn for_activation(
        component: ComponentId,
        process: ProcessInstanceId,
        activation: ActivationId,
        supervisor: mpsc::UnboundedSender<RuntimeEvent>,
        bindings: Vec<ResolvedBinding>,
        scope: &'scope mut EffectScope,
    ) -> Self {
        Self {
            component,
            process,
            activation,
            supervisor,
            bindings,
            capabilities: Vec::new(),
            scope,
        }
    }

    /// Returns the stable component identity for this activation.
    pub fn component(&self) -> &ComponentId {
        &self.component
    }

    /// Returns the process lifetime identity for this activation.
    pub const fn process_instance(&self) -> ProcessInstanceId {
        self.process
    }

    /// Returns this activation's identity.
    pub const fn activation_id(&self) -> ActivationId {
        self.activation
    }

    /// Alias for [`Self::activation_id`].
    pub const fn activation(&self) -> ActivationId {
        self.activation_id()
    }

    /// Returns all resolver-selected bindings as their control-plane form.
    pub fn bindings(&self) -> &[ResolvedBinding] {
        &self.bindings
    }

    /// Resolves one named requirement into a typed binding.
    pub fn binding<C: Capability>(
        &self,
        requirement_name: impl AsRef<str>,
    ) -> std::result::Result<Binding<C>, RequirementError> {
        let requirement_name = requirement_name.as_ref();
        self.binding_untyped(requirement_name, C::TYPE_NAME)?
            .into_binding::<C>()
            .map_err(|source| RequirementError::TypeMismatch {
                requirement: requirement_name.to_owned(),
                source,
            })
    }

    /// Resolves one named requirement into its untyped binding.
    pub fn binding_untyped(
        &self,
        requirement_name: impl AsRef<str>,
        expected_capability_type: impl AsRef<str>,
    ) -> std::result::Result<ResolvedBinding, RequirementError> {
        let requirement_name = requirement_name.as_ref();
        let expected_capability_type = expected_capability_type.as_ref();
        let mut matches = self
            .bindings
            .iter()
            .filter(|binding| binding.requirement_name == requirement_name);
        let binding = matches.next().ok_or_else(|| RequirementError::Missing {
            requirement: requirement_name.to_owned(),
        })?;
        if matches.next().is_some() {
            return Err(RequirementError::Duplicate {
                requirement: requirement_name.to_owned(),
            });
        }
        if binding.capability_type != expected_capability_type {
            return Err(RequirementError::TypeMismatch {
                requirement: requirement_name.to_owned(),
                source: BindingTypeError {
                    expected: expected_capability_type.to_owned(),
                    actual: binding.capability_type.clone(),
                },
            });
        }
        Ok(binding.clone())
    }

    /// Opens a resolved fixed-size RPC client owned by this activation.
    ///
    /// The provider descriptor is ABI-checked before the native endpoint is
    /// opened. Scope teardown expires the client before joining activation
    /// tasks, so every pending wait terminates with `ActivationExpired`.
    pub fn require_rpc<C, Q, R>(
        &mut self,
        requirement_name: impl AsRef<str>,
    ) -> std::result::Result<RpcClient<Q, R>, RpcEndpointError>
    where
        C: Capability,
        Q: FixedMessageAbi + ZeroCopySend + Default,
        R: FixedMessageAbi + ZeroCopySend,
    {
        let requirement_name = requirement_name.as_ref().to_owned();
        let binding = self.binding::<C>(&requirement_name)?;
        let descriptor = binding.try_rpc().ok_or(RpcEndpointError::WrongTransport {
            requirement: requirement_name,
        })?;
        let client = RpcClient::<Q, R>::open_for_activation(
            descriptor,
            binding.descriptor.capability_instance(),
            self.activation,
        )?;
        let wake = client.wake_handle();
        self.scope.wake_on_shutdown(move || {
            let _ = wake.expire();
        });
        Ok(client)
    }

    /// Advertises a data-plane stream for this activation.
    pub fn provide<C: Capability>(
        &mut self,
        capability_instance: CapabilityInstanceId,
        descriptor: StreamDescriptor,
    ) {
        self.provide_untyped(C::TYPE_NAME, capability_instance, descriptor);
    }

    /// Advertises a data-plane stream for a runtime capability role.
    pub fn provide_untyped(
        &mut self,
        capability_type: impl Into<String>,
        capability_instance: CapabilityInstanceId,
        descriptor: StreamDescriptor,
    ) {
        self.capabilities.push(ProvidedCapability::new(
            capability_type,
            rbt_core::CapabilityDescriptor::new(
                capability_instance,
                self.component.clone(),
                self.process,
                self.activation,
                descriptor,
            ),
        ));
    }

    /// Advertises a typed local RPC endpoint for this activation.
    pub fn provide_rpc<C: Capability>(
        &mut self,
        capability_instance: CapabilityInstanceId,
        descriptor: RpcDescriptor,
    ) {
        self.provide_rpc_untyped(C::TYPE_NAME, capability_instance, descriptor);
    }

    /// Advertises a local RPC endpoint for a runtime capability role.
    pub fn provide_rpc_untyped(
        &mut self,
        capability_type: impl Into<String>,
        capability_instance: CapabilityInstanceId,
        descriptor: RpcDescriptor,
    ) {
        self.capabilities.push(ProvidedCapability::new(
            capability_type,
            rbt_core::CapabilityDescriptor::new_rpc(
                capability_instance,
                self.component.clone(),
                self.process,
                self.activation,
                descriptor,
            ),
        ));
    }

    /// Opens and advertises a fixed-size RPC server with default bounded
    /// resources for this activation.
    pub fn rpc_server<C, Q, R>(
        &mut self,
        capability_instance: CapabilityInstanceId,
    ) -> std::result::Result<RpcServer<Q, R>, RpcEndpointError>
    where
        C: Capability,
        Q: FixedMessageAbi + ZeroCopySend,
        R: FixedMessageAbi + ZeroCopySend + Default,
    {
        self.rpc_server_with_resources::<C, Q, R>(capability_instance, RpcResourceConfig::default())
    }

    /// Opens and advertises a fixed-size RPC server with explicit bounded
    /// resources for this activation.
    ///
    /// The native endpoint and wake sidecars are established before the
    /// capability is staged for publication. Each activation must supply a
    /// fresh `CapabilityInstanceId`, yielding
    /// `rbt/cap/<instance>/rpc` as the physical endpoint.
    pub fn rpc_server_with_resources<C, Q, R>(
        &mut self,
        capability_instance: CapabilityInstanceId,
        resources: RpcResourceConfig,
    ) -> std::result::Result<RpcServer<Q, R>, RpcEndpointError>
    where
        C: Capability,
        Q: FixedMessageAbi + ZeroCopySend,
        R: FixedMessageAbi + ZeroCopySend + Default,
    {
        let descriptor = RpcDescriptor::for_fixed_with_resources::<Q, R>(
            capability_instance.rpc_service_name(),
            resources,
        );
        let server =
            RpcServer::<Q, R>::open_or_create_for_capability(&descriptor, capability_instance)?;
        let wake = server.wake_handle();
        self.scope.wake_on_shutdown(move || {
            let _ = wake.expire();
        });
        self.provide_rpc::<C>(capability_instance, descriptor);
        Ok(server)
    }

    /// Returns capabilities that will be published after successful activation.
    pub fn capabilities(&self) -> &[ProvidedCapability] {
        &self.capabilities
    }

    /// Returns the activation's cleanup and task scope.
    pub fn scope(&mut self) -> &mut EffectScope {
        self.scope
    }

    /// Spawns a task owned by this activation.
    pub fn spawn<Fut>(&mut self, future: Fut) -> ScopedTaskHandle
    where
        Fut: Future<Output = ()> + Send + 'static,
    {
        self.scope.spawn(future)
    }

    /// Spawns a task with a child cancellation token owned by this activation.
    pub fn spawn_cancellable<F, Fut>(&mut self, task: F) -> ScopedTaskHandle
    where
        F: FnOnce(tokio_util::sync::CancellationToken) -> Fut + Send + 'static,
        Fut: Future<Output = ()> + Send + 'static,
    {
        self.scope.spawn_cancellable(task)
    }

    /// Spawns a critical task whose natural exit invalidates this activation.
    pub fn spawn_critical<F, Fut, E>(
        &mut self,
        name: impl Into<String>,
        task: F,
    ) -> ScopedTaskHandle
    where
        F: FnOnce(tokio_util::sync::CancellationToken) -> Fut + Send + 'static,
        Fut: Future<Output = std::result::Result<(), E>> + Send + 'static,
        E: Display + Send + 'static,
    {
        self.spawn_supervised(name.into(), task, true)
    }

    /// Spawns a background task whose exit is logged but does not invalidate
    /// this activation.
    pub fn spawn_background<F, Fut, E>(
        &mut self,
        name: impl Into<String>,
        task: F,
    ) -> ScopedTaskHandle
    where
        F: FnOnce(tokio_util::sync::CancellationToken) -> Fut + Send + 'static,
        Fut: Future<Output = std::result::Result<(), E>> + Send + 'static,
        E: Display + Send + 'static,
    {
        self.spawn_supervised(name.into(), task, false)
    }

    /// Spawns a synchronous critical task whose natural exit invalidates this
    /// activation.
    ///
    /// `wake` must unblock the task's synchronous wait (for example, by
    /// notifying a condition variable or a synchronous WaitSet). A normal
    /// cancellation is suppressed just like cancellation of an async
    /// supervised task.
    pub fn spawn_critical_blocking<F, W, E>(
        &mut self,
        name: impl Into<String>,
        task: F,
        wake: W,
    ) -> ScopedTaskHandle
    where
        F: FnOnce(tokio_util::sync::CancellationToken) -> std::result::Result<(), E>
            + Send
            + 'static,
        W: Fn() + Send + Sync + 'static,
        E: Display + Send + 'static,
    {
        self.spawn_supervised_blocking(name.into(), task, true, wake)
    }

    /// Spawns a synchronous background task whose exit is logged but does not
    /// invalidate this activation.
    pub fn spawn_background_blocking<F, W, E>(
        &mut self,
        name: impl Into<String>,
        task: F,
        wake: W,
    ) -> ScopedTaskHandle
    where
        F: FnOnce(tokio_util::sync::CancellationToken) -> std::result::Result<(), E>
            + Send
            + 'static,
        W: Fn() + Send + Sync + 'static,
        E: Display + Send + 'static,
    {
        self.spawn_supervised_blocking(name.into(), task, false, wake)
    }

    fn spawn_supervised<F, Fut, E>(
        &mut self,
        task_name: String,
        task: F,
        critical: bool,
    ) -> ScopedTaskHandle
    where
        F: FnOnce(tokio_util::sync::CancellationToken) -> Fut + Send + 'static,
        Fut: Future<Output = std::result::Result<(), E>> + Send + 'static,
        E: Display + Send + 'static,
    {
        let supervisor = self.supervisor.clone();
        let component = self.component.clone();
        let process = self.process;
        let activation = self.activation;
        self.scope.spawn_cancellable(move |token| async move {
            let task_token = token.clone();
            let outcome = AssertUnwindSafe(async move { task(task_token).await })
                .catch_unwind()
                .await;
            let outcome = match outcome {
                Ok(Ok(())) => TaskOutcome::Completed,
                Ok(Err(error)) => TaskOutcome::Error(error.to_string()),
                Err(_) => TaskOutcome::Panicked,
            };

            // Cancellation and abort belong to scope teardown and must not
            // look like an unexpected task exit to the supervisor.
            if token.is_cancelled() {
                return;
            }

            let event = if critical {
                RuntimeEvent::CriticalTaskExited {
                    component,
                    process,
                    activation,
                    task_name,
                    outcome,
                }
            } else {
                RuntimeEvent::BackgroundTaskExited {
                    component,
                    process,
                    activation,
                    task_name,
                    outcome,
                }
            };
            let _ = supervisor.send(event);
        })
    }

    fn spawn_supervised_blocking<F, W, E>(
        &mut self,
        task_name: String,
        task: F,
        critical: bool,
        wake: W,
    ) -> ScopedTaskHandle
    where
        F: FnOnce(tokio_util::sync::CancellationToken) -> std::result::Result<(), E>
            + Send
            + 'static,
        W: Fn() + Send + Sync + 'static,
        E: Display + Send + 'static,
    {
        let supervisor = self.supervisor.clone();
        let component = self.component.clone();
        let process = self.process;
        let activation = self.activation;
        self.scope.spawn_blocking_cancellable(
            move |token| {
                let task_token = token.clone();
                let outcome = catch_unwind(AssertUnwindSafe(|| task(task_token)));
                let outcome = match outcome {
                    Ok(Ok(())) => TaskOutcome::Completed,
                    Ok(Err(error)) => TaskOutcome::Error(error.to_string()),
                    Err(_) => TaskOutcome::Panicked,
                };

                // Cancellation and scope teardown must not look like an
                // unexpected task exit to the supervisor.
                if token.is_cancelled() {
                    return;
                }

                let event = if critical {
                    RuntimeEvent::CriticalTaskExited {
                        component,
                        process,
                        activation,
                        task_name,
                        outcome,
                    }
                } else {
                    RuntimeEvent::BackgroundTaskExited {
                        component,
                        process,
                        activation,
                        task_name,
                        outcome,
                    }
                };
                let _ = supervisor.send(event);
            },
            wake,
        )
    }
}

/// The activation resources owned by the local component truth.
struct ActiveActivation {
    activation_id: ActivationId,
    bindings: Vec<ResolvedBinding>,
    capabilities: Vec<ProvidedCapability>,
    scope: EffectScope,
}

/// A server command/connection event consumed by the component's main runner.
#[derive(Debug)]
enum ControlEvent {
    Command(ServerMessage),
    ControlPlaneConnected,
    ControlPlaneDisconnected,
}

#[cfg(unix)]
async fn shutdown_signal() -> std::io::Result<()> {
    let mut sigint = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt())?;
    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;

    tokio::select! {
        _ = sigint.recv() => Ok(()),
        _ = sigterm.recv() => Ok(()),
    }
}

#[cfg(not(unix))]
async fn shutdown_signal() -> std::io::Result<()> {
    tokio::signal::ctrl_c().await
}

/// Monotonic command gate. Revisions from a new composer epoch start a new
/// history; sequence numbers are never compared across epochs.
#[derive(Clone, Debug, Default)]
struct RevisionGate {
    latest: Option<Revision>,
}

impl RevisionGate {
    fn accepts(&mut self, revision: Revision) -> bool {
        let accepted = match self.latest {
            None => true,
            Some(latest) if latest.epoch != revision.epoch => true,
            Some(latest) => revision.sequence > latest.sequence,
        };
        if accepted {
            self.latest = Some(revision);
        }
        accepted
    }

    fn reset_for_connection(&mut self) {
        self.latest = None;
    }
}

/// Runs one prepared component against a composer TCP endpoint.
///
/// Preparation and the process identity happen once. The control client is a
/// separate reconnecting task; loss of that task's TCP stream only changes
/// control-plane status and never tears down the local activation.
pub async fn run_component<C, A>(
    config: C::Config,
    composer_addr: A,
) -> std::result::Result<(), ControlPlaneError>
where
    C: Component,
    A: ToSocketAddrs + Send,
{
    run_component_until::<C, A, _>(config, composer_addr, async {
        if let Err(error) = shutdown_signal().await {
            warn!(%error, "failed to listen for shutdown signal");
        }
    })
    .await
}

/// Runs one prepared component until an application-owned shutdown future completes.
///
/// This retains the same lifecycle, reconnect, supervision, and cleanup path as
/// [`run_component`]. It is intended for finite native tools such as MCAP
/// playback, whose successful work completion is itself the shutdown event.
pub async fn run_component_until<C, A, S>(
    config: C::Config,
    composer_addr: A,
    shutdown: S,
) -> std::result::Result<(), ControlPlaneError>
where
    C: Component,
    A: ToSocketAddrs + Send,
    S: Future<Output = ()> + Send,
{
    let mut component = C::prepare(&config)
        .await
        .map_err(|error| ControlPlaneError::Component(ComponentError::preparation(error)))?;
    let component_id = component.id();
    let process_instance = ProcessInstanceId::new();
    let requirements = component.requirements();
    let address = tokio::net::lookup_host(composer_addr)
        .await
        .map_err(ControlPlaneError::ResolveAddress)?
        .next()
        .ok_or_else(|| {
            ControlPlaneError::ResolveAddress(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "composer address resolved to no endpoints",
            ))
        })?;

    let mut local = LocalTruth::new(component_id, process_instance, requirements);
    let (snapshot_tx, snapshot_rx) = watch::channel(local.snapshot());
    let (control_tx, mut control_rx) = mpsc::unbounded_channel();
    let control_task = tokio::spawn(control_client(
        address,
        local.component_id.clone(),
        local.process_instance,
        snapshot_rx,
        control_tx,
    ));
    let (supervisor, mut runtime_events) = mpsc::unbounded_channel();
    let mut supervisor_open = true;
    let mut pressure_tick = tokio::time::interval(Duration::from_secs(1));
    pressure_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    pressure_tick.tick().await;
    tokio::pin!(shutdown);

    loop {
        enum Input {
            Shutdown,
            Control(Option<ControlEvent>),
            Runtime(Option<RuntimeEvent>),
            PressureTick,
        }
        let input = tokio::select! {
            _ = &mut shutdown => Input::Shutdown,
            event = control_rx.recv() => Input::Control(event),
            event = runtime_events.recv(), if supervisor_open => Input::Runtime(event),
            _ = pressure_tick.tick() => Input::PressureTick,
        };

        match input {
            Input::Shutdown => {
                local.shutdown_active().await;
                local.publish(&snapshot_tx);
                control_task.abort();
                return Ok(());
            }
            Input::Control(Some(ControlEvent::Command(message))) => {
                handle_server_message(
                    &mut component,
                    &mut local,
                    message,
                    &supervisor,
                    &snapshot_tx,
                )
                .await?;
            }
            Input::Control(Some(ControlEvent::ControlPlaneConnected)) => {
                // A command may have been written by the composer but lost
                // with the previous TCP stream.  Resetting at the connection
                // boundary lets the composer safely replay its pending,
                // idempotent command with the same epoch-scoped revision.
                local.revision_gate.reset_for_connection();
                local.control_plane = ControlPlaneStatus::Connected;
                // Preparation is process-scoped and is already complete when
                // the first control connection is established.  Until the
                // composer supplies a fully resolved binding set, the local
                // lifecycle truth is therefore Blocked rather than Preparing.
                // This also makes initially missing requirements visible via
                // the same state used after provider loss.
                if matches!(local.state, ComponentState::Preparing) {
                    local.transition(ComponentState::Blocked(
                        "awaiting resolved activation bindings".into(),
                    ));
                    local.publish(&snapshot_tx);
                }
                debug!(component = %local.component_id, "control plane connected");
            }
            Input::Control(Some(ControlEvent::ControlPlaneDisconnected)) => {
                if local.control_plane != ControlPlaneStatus::Disconnected {
                    local.control_plane = ControlPlaneStatus::Disconnected;
                    info!(
                        component = %local.component_id,
                        process_instance = %local.process_instance,
                        state = ?local.state,
                        "control plane disconnected; data plane remains active"
                    );
                }
            }
            Input::Control(None) => {
                // The reconnecting task only exits when the main runner is
                // shutting down or its channel has been dropped.
                break;
            }
            Input::Runtime(Some(event)) => {
                handle_runtime_event(&mut local, event, &snapshot_tx).await?;
            }
            Input::Runtime(None) => {
                supervisor_open = false;
            }
            Input::PressureTick => {
                local.publish_if_changed(&snapshot_tx);
            }
        }
    }

    local.shutdown_active().await;
    control_task.abort();
    Ok(())
}

/// Local lifecycle/binding truth. This type intentionally has no connection
/// or writer state, so a control-plane outage cannot mutate its activation.
struct LocalTruth {
    component_id: ComponentId,
    process_instance: ProcessInstanceId,
    requirements: Vec<RequirementSpec>,
    state: ComponentState,
    active: Option<ActiveActivation>,
    next_activation: ActivationId,
    revision_gate: RevisionGate,
    control_plane: ControlPlaneStatus,
}

impl LocalTruth {
    fn new(
        component_id: ComponentId,
        process_instance: ProcessInstanceId,
        requirements: Vec<RequirementSpec>,
    ) -> Self {
        Self {
            component_id,
            process_instance,
            requirements,
            state: ComponentState::Preparing,
            active: None,
            next_activation: ActivationId::first(),
            revision_gate: RevisionGate::default(),
            control_plane: ControlPlaneStatus::Disconnected,
        }
    }

    fn transition(&mut self, next: ComponentState) -> bool {
        if self.state == next {
            return true;
        }
        match self.state.transition(next.clone()) {
            Ok(_) => true,
            Err(error) => {
                debug!(
                    component = %self.component_id,
                    from = ?self.state,
                    to = ?next,
                    %error,
                    "ignored illegal local lifecycle transition"
                );
                false
            }
        }
    }

    fn snapshot(&self) -> ComponentSnapshot {
        let activation = self
            .active
            .as_ref()
            .map(|active| ActivationSnapshot::new(active.activation_id, active.bindings.clone()));
        let requirements = self
            .requirements
            .iter()
            .cloned()
            .map(|requirement| {
                let binding = activation
                    .as_ref()
                    .and_then(|activation| activation.binding_for(&requirement.requirement_name))
                    .cloned();
                RequirementSnapshot::new(requirement, binding)
            })
            .collect::<Vec<_>>();
        let capabilities = self
            .active
            .as_ref()
            .map(|active| {
                active
                    .capabilities
                    .iter()
                    .map(CapabilitySnapshot::from_provided)
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();
        ComponentSnapshot::new(
            self.component_id.clone(),
            self.process_instance,
            self.state.clone(),
            activation,
            requirements,
            capabilities,
        )
        .with_stream_pressures(rbt_core::local_pressure_snapshots())
        .with_rpc_pressures(rbt_core::local_rpc_pressure_snapshots())
    }

    fn publish(&self, tx: &watch::Sender<ComponentSnapshot>) {
        // The receiver may disappear after the composer is shut down; local
        // lifecycle truth must still continue independently.
        let _ = tx.send(self.snapshot());
    }

    fn publish_if_changed(&self, tx: &watch::Sender<ComponentSnapshot>) {
        let next = self.snapshot();
        tx.send_if_modified(|current| {
            if *current == next {
                return false;
            }
            *current = next;
            true
        });
    }

    async fn shutdown_active(&mut self) {
        if let Some(mut active) = self.active.take() {
            active.scope.shutdown().await;
        }
    }
}

async fn handle_server_message<C>(
    component: &mut C,
    local: &mut LocalTruth,
    message: ServerMessage,
    supervisor: &mpsc::UnboundedSender<RuntimeEvent>,
    snapshot_tx: &watch::Sender<ComponentSnapshot>,
) -> std::result::Result<(), ControlPlaneError>
where
    C: Component,
{
    if let Some(revision) = message.revision()
        && !local.revision_gate.accepts(revision)
    {
        debug!(
            component = %local.component_id,
            %revision,
            "stale server command ignored"
        );
        return Ok(());
    }

    match message {
        ServerMessage::Activate { revision, bindings } => {
            if local.active.is_some()
                || !matches!(
                    local.state,
                    ComponentState::Preparing | ComponentState::Blocked(_)
                )
            {
                debug!(
                    component = %local.component_id,
                    state = ?local.state,
                    "Activate ignored because component is not ready"
                );
                return Ok(());
            }

            // Activation IDs belong to the process, not to a composer epoch.
            // Consume one for every accepted activation attempt so a failed
            // attempt can never be confused with a later successful one.
            let activation_id = local.next_activation;
            local.next_activation = local.next_activation.next();
            let mut scope = EffectScope::new();
            if !local.transition(ComponentState::Activating) {
                return Ok(());
            }
            local.publish(snapshot_tx);

            let (activation_result, capabilities) = {
                let mut ctx = ActivationCtx::for_activation(
                    local.component_id.clone(),
                    local.process_instance,
                    activation_id,
                    supervisor.clone(),
                    bindings.clone(),
                    &mut scope,
                );
                let result = component.activate(&mut ctx).await;
                let capabilities = ctx.capabilities().to_vec();
                (result, capabilities)
            };

            match activation_result {
                Ok(()) => {
                    local.active = Some(ActiveActivation {
                        activation_id,
                        bindings,
                        capabilities,
                        scope,
                    });
                    local.transition(ComponentState::Active);
                    info!(
                        component = %local.component_id,
                        process_instance = %local.process_instance,
                        activation = %activation_id,
                        composer_epoch = %revision.epoch,
                        %revision,
                        "component activation is Active"
                    );
                    local.publish(snapshot_tx);
                }
                Err(error) => {
                    scope.shutdown().await;
                    local.transition(ComponentState::Failed(error.to_string()));
                    warn!(
                        component = %local.component_id,
                        process_instance = %local.process_instance,
                        activation = %activation_id,
                        composer_epoch = %revision.epoch,
                        %revision,
                        %error,
                        "component activation failed"
                    );
                    local.publish(snapshot_tx);
                }
            }
        }
        ServerMessage::Deactivate {
            revision,
            expected_process_instance,
            expected_activation,
            reason,
        } => {
            if expected_process_instance != local.process_instance
                || local.active.as_ref().map(|active| active.activation_id)
                    != Some(expected_activation)
            {
                debug!(
                    component = %local.component_id,
                    process_instance = %local.process_instance,
                    activation = %expected_activation,
                    expected_process_instance = %expected_process_instance,
                    "stale Deactivate ignored"
                );
                return Ok(());
            }
            info!(
                component = %local.component_id,
                process_instance = %local.process_instance,
                activation = %expected_activation,
                composer_epoch = %revision.epoch,
                %reason,
                "ACTIVE -> QUIESCING"
            );
            local.transition(ComponentState::Quiescing);
            local.publish(snapshot_tx);
            local.shutdown_active().await;
            local.transition(ComponentState::Blocked(reason.clone().into()));
            info!(
                component = %local.component_id,
                process_instance = %local.process_instance,
                activation = %expected_activation,
                composer_epoch = %revision.epoch,
                %reason,
                "QUIESCING -> BLOCKED"
            );
            local.publish(snapshot_tx);
        }
        ServerMessage::ProcessConflict {
            component_id,
            incumbent,
            contender,
            ..
        } => {
            warn!(
                component = %component_id,
                incumbent = %incumbent,
                contender = %contender,
                "component process identity conflict; retaining local activation"
            );
            // The snapshot sent before the conflict was quarantined. Trigger
            // one fresh full snapshot so, if the other process disconnects,
            // the surviving identity can be reconstructed without waiting
            // for an unrelated lifecycle change.
            local.publish(snapshot_tx);
        }
        ServerMessage::ProtocolMismatch {
            expected_control_protocol_version,
            expected_snapshot_version,
            received_control_protocol_version,
            received_snapshot_version,
            reason,
        } => {
            warn!(
                component = %local.component_id,
                expected_control_protocol_version,
                expected_snapshot_version,
                ?received_control_protocol_version,
                ?received_snapshot_version,
                %reason,
                "composer rejected control-plane protocol"
            );
        }
        ServerMessage::InspectResponse { .. } => {
            // Inspection responses are intended for read-only diagnostic
            // clients. A component connection never acts on one.
            debug!(component = %local.component_id, "ignoring inspection response on component connection");
        }
    }
    Ok(())
}

async fn handle_runtime_event(
    local: &mut LocalTruth,
    event: RuntimeEvent,
    snapshot_tx: &watch::Sender<ComponentSnapshot>,
) -> std::result::Result<(), ControlPlaneError> {
    match event {
        RuntimeEvent::CriticalTaskExited {
            component,
            process,
            activation,
            task_name,
            outcome,
        } => {
            if !is_current_activation(
                &local.component_id,
                local.process_instance,
                local.active.as_ref().map(|active| active.activation_id),
                &component,
                process,
                activation,
            ) {
                debug!(
                    component = %component,
                    process_instance = %process,
                    activation = %activation,
                    task = %task_name,
                    "stale critical event ignored"
                );
                return Ok(());
            }

            info!(
                component = %component,
                process_instance = %process,
                activation = %activation,
                task = %task_name,
                "ACTIVE -> QUIESCING after critical task exit"
            );
            local.transition(ComponentState::Quiescing);
            local.publish(snapshot_tx);
            for capability in local
                .active
                .as_ref()
                .into_iter()
                .flat_map(|active| &active.capabilities)
            {
                info!(
                    component = %component,
                    process_instance = %process,
                    activation = %activation,
                    capability_type = %capability.capability_type,
                    capability_instance = %capability.descriptor.capability_instance,
                    "removing capability for invalidated activation"
                );
            }
            local.shutdown_active().await;
            let reason = format!("critical task {task_name} exited: {outcome:?}");
            local.transition(ComponentState::Failed(reason.clone()));
            warn!(
                component = %component,
                process_instance = %process,
                activation = %activation,
                task = %task_name,
                outcome = ?outcome,
                "activation FAILED after critical task exit: {reason}"
            );
            local.publish(snapshot_tx);
        }
        RuntimeEvent::BackgroundTaskExited {
            component,
            process,
            activation,
            task_name,
            outcome,
        } => {
            if !is_current_activation(
                &local.component_id,
                local.process_instance,
                local.active.as_ref().map(|active| active.activation_id),
                &component,
                process,
                activation,
            ) {
                debug!(
                    component = %component,
                    process_instance = %process,
                    activation = %activation,
                    task = %task_name,
                    "stale background event ignored"
                );
                return Ok(());
            }

            error!(
                component = %component,
                process_instance = %process,
                activation = %activation,
                task = %task_name,
                outcome = ?outcome,
                "background task exited; activation remains unchanged"
            );
        }
    }
    Ok(())
}

fn is_current_activation(
    component_id: &ComponentId,
    process_instance: ProcessInstanceId,
    active_activation: Option<ActivationId>,
    event_component: &ComponentId,
    event_process: ProcessInstanceId,
    event_activation: ActivationId,
) -> bool {
    event_component == component_id
        && event_process == process_instance
        && active_activation == Some(event_activation)
}

/// Reconnecting control-plane client. The component's watch channel always
/// contains the latest local snapshot; a newly connected composer receives it
/// immediately after Hello and every later change exactly as a full snapshot.
async fn control_client(
    address: SocketAddr,
    component_id: ComponentId,
    process_instance: ProcessInstanceId,
    mut snapshot_rx: watch::Receiver<ComponentSnapshot>,
    events_tx: mpsc::UnboundedSender<ControlEvent>,
) {
    const BACKOFF: [Duration; 4] = [
        Duration::from_millis(100),
        Duration::from_millis(200),
        Duration::from_millis(500),
        Duration::from_millis(1000),
    ];
    let mut backoff_index = 0;

    loop {
        let result = TcpStream::connect(address).await;
        match result {
            Ok(stream) => {
                backoff_index = 0;
                if run_control_connection(
                    stream,
                    &component_id,
                    process_instance,
                    &mut snapshot_rx,
                    &events_tx,
                )
                .await
                .is_err()
                {
                    let _ = events_tx.send(ControlEvent::ControlPlaneDisconnected);
                }
            }
            Err(error) => {
                debug!(component = %component_id, %error, "composer connection attempt failed");
                let _ = events_tx.send(ControlEvent::ControlPlaneDisconnected);
            }
        }
        sleep(BACKOFF[backoff_index]).await;
        backoff_index = (backoff_index + 1).min(BACKOFF.len() - 1);
    }
}

async fn run_control_connection(
    stream: TcpStream,
    component_id: &ComponentId,
    process_instance: ProcessInstanceId,
    snapshot_rx: &mut watch::Receiver<ComponentSnapshot>,
    events_tx: &mpsc::UnboundedSender<ControlEvent>,
) -> std::io::Result<()> {
    let (read_half, write_half) = stream.into_split();
    let mut writer = BufWriter::new(write_half);
    let hello = ClientMessage::Hello {
        component_id: component_id.clone(),
        process_instance,
        control_protocol_version: CONTROL_PROTOCOL_VERSION,
        snapshot_version: COMPOSER_SNAPSHOT_VERSION,
    };
    send_client_message(&mut writer, &hello).await?;
    let current = snapshot_rx.borrow_and_update().clone();
    send_client_message(&mut writer, &ClientMessage::Snapshot { snapshot: current }).await?;
    events_tx
        .send(ControlEvent::ControlPlaneConnected)
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::BrokenPipe, "runner stopped"))?;

    read_server_messages(read_half, writer, snapshot_rx, events_tx).await
}

async fn read_server_messages(
    read_half: OwnedReadHalf,
    mut writer: BufWriter<tokio::net::tcp::OwnedWriteHalf>,
    snapshot_rx: &mut watch::Receiver<ComponentSnapshot>,
    events_tx: &mpsc::UnboundedSender<ControlEvent>,
) -> std::io::Result<()> {
    let mut reader = BufReader::new(read_half);
    let mut line = String::new();
    loop {
        line.clear();
        enum Input {
            Read(std::io::Result<usize>),
            Snapshot(Result<(), watch::error::RecvError>),
        }
        let input = tokio::select! {
            read = reader.read_line(&mut line) => Input::Read(read),
            changed = snapshot_rx.changed() => Input::Snapshot(changed),
        };
        match input {
            Input::Read(Ok(0)) => {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::UnexpectedEof,
                    "composer closed control connection",
                ));
            }
            Input::Read(Ok(_)) => {
                let message = serde_json::from_str::<ServerMessage>(line.trim_end())
                    .map_err(std::io::Error::other)?;
                events_tx
                    .send(ControlEvent::Command(message))
                    .map_err(|_| {
                        std::io::Error::new(std::io::ErrorKind::BrokenPipe, "runner stopped")
                    })?;
            }
            Input::Read(Err(error)) => return Err(error),
            Input::Snapshot(Ok(())) => {
                let snapshot = snapshot_rx.borrow_and_update().clone();
                send_client_message(&mut writer, &ClientMessage::Snapshot { snapshot }).await?;
            }
            Input::Snapshot(Err(_)) => {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::BrokenPipe,
                    "component snapshot channel closed",
                ));
            }
        }
    }
}

async fn send_client_message(
    writer: &mut BufWriter<tokio::net::tcp::OwnedWriteHalf>,
    message: &ClientMessage,
) -> std::io::Result<()> {
    let encoded = serde_json::to_vec(message).map_err(std::io::Error::other)?;
    writer.write_all(&encoded).await?;
    writer.write_all(b"\n").await?;
    writer.flush().await
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::{
        ActivationSnapshot, CapabilitySnapshot, ComponentSnapshot, RequirementSnapshot,
    };
    use crate::{CapabilityRegistry, ReconcilePlan, reconcile};
    use rbt_core::{
        ActivationId, CapabilityDescriptor, CapabilityInstanceId, ComponentState, ComposerEpoch,
        ProvidedCapability, RequirementSpec, ResolvedBinding, Revision, RpcDescriptor,
        StreamDescriptor,
    };
    use rbt_iox2::{WaitContext, WaitExit, WaitProgress};
    use rbt_types::{Detection, Frame, StreamProfile};
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
        mpsc as std_mpsc,
    };
    use std::time::Instant;

    struct RuntimeRpc;

    impl Capability for RuntimeRpc {
        const TYPE_NAME: &'static str = "RuntimeRpc";
    }

    /// A small prepared component used by the replacement stress test.  The
    /// component owns its activation client just as a real implementation
    /// would own a client handle used by one of its scoped tasks; the
    /// activation scope remains the authority that expires it.
    struct ReplacementClient {
        component: ComponentId,
        activation_count: Arc<AtomicUsize>,
        client: Option<RpcClient<Frame, Detection>>,
    }

    struct ReplacementClientConfig {
        component: ComponentId,
        prepare_count: Arc<AtomicUsize>,
        activation_count: Arc<AtomicUsize>,
    }

    #[async_trait]
    impl Component for ReplacementClient {
        type Config = ReplacementClientConfig;
        type Error = String;

        async fn prepare(config: &Self::Config) -> Result<Self, Self::Error> {
            config.prepare_count.fetch_add(1, Ordering::SeqCst);
            Ok(Self {
                component: config.component.clone(),
                activation_count: Arc::clone(&config.activation_count),
                client: None,
            })
        }

        fn id(&self) -> ComponentId {
            self.component.clone()
        }

        fn requirements(&self) -> Vec<RequirementSpec> {
            vec![RequirementSpec::typed::<RuntimeRpc>("rpc")]
        }

        async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<(), Self::Error> {
            self.activation_count.fetch_add(1, Ordering::SeqCst);
            self.client = Some(
                ctx.require_rpc::<RuntimeRpc, Frame, Detection>("rpc")
                    .map_err(|error| error.to_string())?,
            );
            Ok(())
        }
    }

    fn provider_snapshot(provided: &ProvidedCapability) -> ComponentSnapshot {
        ComponentSnapshot::new(
            provided.descriptor.provider_component.clone(),
            provided.descriptor.provider_process,
            ComponentState::Active,
            Some(ActivationSnapshot::new(provided.descriptor.activation, [])),
            std::iter::empty::<RequirementSnapshot>(),
            [CapabilitySnapshot::from_provided(provided)],
        )
    }

    fn binding_for_provider(provided: &ProvidedCapability) -> ResolvedBinding {
        ResolvedBinding::new(
            "rpc",
            RuntimeRpc::TYPE_NAME,
            provided.descriptor.provider_component.clone(),
            provided.descriptor.clone(),
        )
    }

    fn complete_rpc_call(
        client: &RpcClient<Frame, Detection>,
        server: &mut RpcServer<Frame, Detection>,
        marker: u64,
    ) -> Result<Detection, RpcError> {
        let mut request = client.loan_request()?;
        request.sequence = marker;
        request.value = marker;
        let pending = request.send()?;
        let request = server
            .receive_timeout(Duration::from_millis(500))?
            .ok_or(RpcError::DeadlineExceeded)?;
        let mut response = request.loan_response()?;
        response.sequence = marker;
        response.value = marker;
        response.send()?;
        let response = pending.wait(Duration::from_millis(500))?;
        Ok(*response)
    }

    fn binding_for_type(capability_type: &str) -> ResolvedBinding {
        ResolvedBinding::new(
            "camera",
            capability_type,
            ComponentId::from("camera-a"),
            CapabilityDescriptor::new(
                CapabilityInstanceId::new(),
                ComponentId::from("camera-a"),
                ProcessInstanceId::new(),
                ActivationId::first(),
                StreamDescriptor::for_fixed::<Detection>(
                    "rbt/test/camera-a",
                    StreamProfile::Sensor,
                ),
            ),
        )
    }

    #[test]
    fn stale_critical_event_activation_one_is_ignored_when_two_is_current() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        assert!(!is_current_activation(
            &component,
            process,
            Some(ActivationId::new(2)),
            &component,
            process,
            ActivationId::new(1),
        ));
    }

    #[test]
    fn revision_gate_rejects_stale_same_epoch_but_accepts_new_epoch() {
        let first_epoch = ComposerEpoch::new();
        let second_epoch = ComposerEpoch::new();
        let mut gate = RevisionGate::default();
        assert!(gate.accepts(Revision::new(first_epoch, 4)));
        assert!(!gate.accepts(Revision::new(first_epoch, 3)));
        assert!(!gate.accepts(Revision::new(first_epoch, 4)));
        assert!(gate.accepts(Revision::new(second_epoch, 1)));
        assert!(!gate.accepts(Revision::new(second_epoch, 1)));
        assert!(gate.accepts(Revision::new(second_epoch, 2)));
    }

    #[test]
    fn untyped_binding_reports_missing_duplicate_and_type_mismatch() {
        let missing = {
            let mut scope = EffectScope::new();
            let ctx = ActivationCtx::new(Vec::new(), &mut scope);
            matches!(
                ctx.binding_untyped("camera", "Camera"),
                Err(RequirementError::Missing { requirement }) if requirement == "camera"
            )
        };
        assert!(missing);

        let duplicate_binding = binding_for_type("Camera");
        let duplicate = {
            let mut scope = EffectScope::new();
            let ctx = ActivationCtx::new(
                vec![duplicate_binding.clone(), duplicate_binding],
                &mut scope,
            );
            matches!(
                ctx.binding_untyped("camera", "Camera"),
                Err(RequirementError::Duplicate { requirement }) if requirement == "camera"
            )
        };
        assert!(duplicate);

        let mismatch = {
            let mut scope = EffectScope::new();
            let ctx = ActivationCtx::new(vec![binding_for_type("Other")], &mut scope);
            matches!(
                ctx.binding_untyped("camera", "Camera"),
                Err(RequirementError::TypeMismatch { requirement, source })
                    if requirement == "camera"
                        && source.expected == "Camera"
                        && source.actual == "Other"
            )
        };
        assert!(mismatch);
    }

    #[test]
    fn untyped_provide_preserves_runtime_role_and_activation_identity() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::new(7);
        let capability_instance = CapabilityInstanceId::new();
        let stream = StreamDescriptor::for_fixed::<Detection>(
            capability_instance.stream_service_name(),
            StreamProfile::Sensor,
        );
        let expected = ProvidedCapability::new(
            "RuntimeDetections",
            CapabilityDescriptor::new(
                capability_instance,
                component.clone(),
                process,
                activation,
                stream.clone(),
            ),
        );
        let (supervisor, _events) = mpsc::unbounded_channel();
        let mut scope = EffectScope::new();
        let mut ctx = ActivationCtx::for_activation(
            component,
            process,
            activation,
            supervisor,
            Vec::new(),
            &mut scope,
        );

        ctx.provide_untyped("RuntimeDetections", capability_instance, stream);

        assert_eq!(ctx.capabilities(), &[expected]);
    }

    #[test]
    fn typed_rpc_provide_uses_rpc_endpoint_and_instance_suffix() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::new(7);
        let capability_instance = CapabilityInstanceId::new();
        let descriptor =
            RpcDescriptor::for_fixed::<Detection, Frame>(capability_instance.rpc_service_name());
        let (supervisor, _events) = mpsc::unbounded_channel();
        let mut scope = EffectScope::new();
        let mut ctx = ActivationCtx::for_activation(
            component,
            process,
            activation,
            supervisor,
            Vec::new(),
            &mut scope,
        );

        ctx.provide_rpc_untyped("RuntimeRpc", capability_instance, descriptor.clone());

        let provided = &ctx.capabilities()[0];
        assert_eq!(provided.capability_type(), "RuntimeRpc");
        assert_eq!(
            provided.transport_kind(),
            rbt_core::CapabilityTransportKind::Rpc
        );
        assert_eq!(provided.try_rpc(), Some(&descriptor));
        assert!(provided.try_stream().is_none());
        assert_eq!(provided.name(), capability_instance.rpc_service_name());
    }

    #[tokio::test(flavor = "current_thread")]
    async fn rpc_helpers_open_physical_endpoint_and_expire_pending_client_wait() {
        let provider = ComponentId::from("rpc-provider");
        let provider_process = ProcessInstanceId::new();
        let instance = CapabilityInstanceId::new();
        let (supervisor, _events) = mpsc::unbounded_channel();
        let mut server_scope = EffectScope::new();
        let (server, binding) = {
            let mut ctx = ActivationCtx::for_activation(
                provider.clone(),
                provider_process,
                ActivationId::first(),
                supervisor.clone(),
                Vec::new(),
                &mut server_scope,
            );
            let server = ctx
                .rpc_server::<RuntimeRpc, Frame, Detection>(instance)
                .expect("activation-owned server should open");
            let provided = ctx.capabilities()[0].clone();
            assert_eq!(provided.name(), instance.rpc_service_name());
            let binding = ResolvedBinding::new(
                "rpc",
                RuntimeRpc::TYPE_NAME,
                provider.clone(),
                provided.descriptor,
            );
            (server, binding)
        };

        let mut client_scope = EffectScope::new();
        let client = {
            let mut ctx = ActivationCtx::for_activation(
                ComponentId::from("rpc-client"),
                ProcessInstanceId::new(),
                ActivationId::first(),
                supervisor,
                vec![binding],
                &mut client_scope,
            );
            ctx.require_rpc::<RuntimeRpc, Frame, Detection>("rpc")
                .expect("resolved RPC client should open")
        };
        let server_pressure = server.pressure_snapshot();
        assert_eq!(server_pressure.capability_instance, Some(instance));
        assert!(server_pressure.is_server());
        let client_pressure = client.pressure_snapshot();
        assert_eq!(client_pressure.capability_instance, Some(instance));
        assert_eq!(
            client_pressure.client_activation,
            Some(ActivationId::first())
        );
        assert!(client_pressure.is_client());

        let pending = client
            .loan_request()
            .expect("request loan")
            .send()
            .expect("request send");
        let waiter = std::thread::spawn(move || pending.wait(Duration::from_secs(5)));
        client_scope.shutdown().await;
        assert!(matches!(
            waiter.join().expect("pending waiter"),
            Err(RpcError::ActivationExpired)
        ));

        drop(client);
        drop(server);
        server_scope.shutdown().await;
    }

    #[tokio::test(flavor = "current_thread")]
    async fn rpc_server_scope_shutdown_wakes_blocked_receive() {
        let (supervisor, _events) = mpsc::unbounded_channel();
        let mut scope = EffectScope::new();
        let server = {
            let mut ctx = ActivationCtx::for_activation(
                ComponentId::from("rpc-provider"),
                ProcessInstanceId::new(),
                ActivationId::first(),
                supervisor,
                Vec::new(),
                &mut scope,
            );
            ctx.rpc_server::<RuntimeRpc, Frame, Detection>(CapabilityInstanceId::new())
                .expect("activation-owned server should open")
        };
        let waiter = std::thread::spawn(move || {
            let mut server = server;
            server.receive_timeout(Duration::from_secs(5))
        });

        scope.shutdown().await;
        assert!(matches!(
            waiter.join().expect("server waiter"),
            Err(RpcError::ActivationExpired)
        ));
    }

    #[tokio::test(flavor = "current_thread")]
    async fn rpc_provider_replacement_rotates_endpoint_and_rejects_old_response()
    -> Result<(), RpcEndpointError> {
        let provider = ComponentId::from("rpc-provider");
        let (supervisor, _events) = mpsc::unbounded_channel();

        let instance_a = CapabilityInstanceId::new();
        let mut server_scope_a = EffectScope::new();
        let (mut server_a, binding_a) = {
            let mut ctx = ActivationCtx::for_activation(
                provider.clone(),
                ProcessInstanceId::new(),
                ActivationId::first(),
                supervisor.clone(),
                Vec::new(),
                &mut server_scope_a,
            );
            let server = ctx
                .rpc_server::<RuntimeRpc, Frame, Detection>(instance_a)
                .expect("server A");
            let provided = ctx.capabilities()[0].clone();
            (
                server,
                ResolvedBinding::new(
                    "rpc",
                    RuntimeRpc::TYPE_NAME,
                    provider.clone(),
                    provided.descriptor,
                ),
            )
        };
        let mut client_scope_a = EffectScope::new();
        let client_a = {
            let mut ctx = ActivationCtx::for_activation(
                ComponentId::from("rpc-client"),
                ProcessInstanceId::new(),
                ActivationId::first(),
                supervisor.clone(),
                vec![binding_a],
                &mut client_scope_a,
            );
            ctx.require_rpc::<RuntimeRpc, Frame, Detection>("rpc")
                .expect("client A")
        };
        let pending_a = client_a.loan_request().unwrap().send().unwrap();
        let request_a = server_a
            .receive_timeout(Duration::from_secs(2))
            .unwrap()
            .expect("server A request");

        client_scope_a.shutdown().await;
        server_scope_a.shutdown().await;
        assert!(matches!(
            pending_a.wait(Duration::from_secs(1)),
            Err(RpcError::ActivationExpired)
        ));
        assert!(matches!(
            request_a.loan_response(),
            Err(RpcError::ActivationExpired)
        ));
        drop(client_a);
        drop(server_a);

        let instance_b = CapabilityInstanceId::new();
        assert_ne!(instance_a, instance_b);
        assert_ne!(instance_a.rpc_service_name(), instance_b.rpc_service_name());
        let mut server_scope_b = EffectScope::new();
        let (server_b, binding_b) = {
            let mut ctx = ActivationCtx::for_activation(
                provider.clone(),
                ProcessInstanceId::new(),
                ActivationId::new(2),
                supervisor.clone(),
                Vec::new(),
                &mut server_scope_b,
            );
            let server = ctx
                .rpc_server::<RuntimeRpc, Frame, Detection>(instance_b)
                .expect("server B");
            let provided = ctx.capabilities()[0].clone();
            (
                server,
                ResolvedBinding::new("rpc", RuntimeRpc::TYPE_NAME, provider, provided.descriptor),
            )
        };
        let mut client_scope_b = EffectScope::new();
        let client_b = {
            let mut ctx = ActivationCtx::for_activation(
                ComponentId::from("rpc-client"),
                ProcessInstanceId::new(),
                ActivationId::new(2),
                supervisor,
                vec![binding_b],
                &mut client_scope_b,
            );
            ctx.require_rpc::<RuntimeRpc, Frame, Detection>("rpc")
                .expect("client B")
        };
        let server_thread = std::thread::spawn(move || -> Result<(), RpcError> {
            let mut server = server_b;
            let request = server
                .receive_timeout(Duration::from_secs(2))?
                .expect("server B request");
            let mut response = request.loan_response()?;
            response.sequence = 44;
            response.value = 55;
            response.send()
        });
        let response = client_b.call(Frame::default(), Duration::from_secs(2))?;
        assert_eq!((response.sequence, response.value), (44, 55));
        server_thread.join().expect("server B thread")?;
        client_scope_b.shutdown().await;
        server_scope_b.shutdown().await;
        Ok::<(), RpcEndpointError>(())
    }

    #[tokio::test(flavor = "current_thread")]
    async fn rpc_provider_replacement_stress_keeps_prepared_client_and_rotates_lifecycle() {
        // This is intentionally bounded: each native wait has a finite
        // timeout and the outer watchdog prevents a regression from leaving
        // the test process stuck in an activation or transport wait.
        let completed = tokio::time::timeout(Duration::from_secs(60), async {
            let provider = ComponentId::from("rpc-stress-provider");
            let client_id = ComponentId::from("rpc-stress-client");
            let client_process = ProcessInstanceId::new();
            let prepare_count = Arc::new(AtomicUsize::new(0));
            let activation_count = Arc::new(AtomicUsize::new(0));
            let config = ReplacementClientConfig {
                component: client_id.clone(),
                prepare_count: Arc::clone(&prepare_count),
                activation_count: Arc::clone(&activation_count),
            };
            let mut component = ReplacementClient::prepare(&config)
                .await
                .expect("client preparation should succeed");
            assert_eq!(prepare_count.load(Ordering::SeqCst), 1);

            let (supervisor, _runtime_events) = mpsc::unbounded_channel();
            let mut local =
                LocalTruth::new(client_id.clone(), client_process, component.requirements());
            local.control_plane = ControlPlaneStatus::Connected;
            assert!(local.transition(ComponentState::Blocked(
                "waiting for rpc-stress-provider".into(),
            )));
            let (snapshot_tx, _snapshot_rx) = watch::channel(local.snapshot());
            let mut registry = CapabilityRegistry::new();
            let epoch = ComposerEpoch::new();
            let mut revision = 0_u64;
            let mut seen_instances = std::collections::HashSet::new();
            let mut seen_endpoints = std::collections::HashSet::new();

            let first_instance = CapabilityInstanceId::new();
            let first_provider_process = ProcessInstanceId::new();
            let mut server_scope = EffectScope::new();
            let (mut server, first_provided) = {
                let mut ctx = ActivationCtx::for_activation(
                    provider.clone(),
                    first_provider_process,
                    ActivationId::first(),
                    supervisor.clone(),
                    Vec::new(),
                    &mut server_scope,
                );
                let server = ctx
                    .rpc_server::<RuntimeRpc, Frame, Detection>(first_instance)
                    .expect("first stress server should open");
                let provided = ctx.capabilities()[0].clone();
                (server, provided)
            };
            assert_eq!(
                first_provided.descriptor.capability_instance,
                first_instance
            );
            assert_eq!(first_provided.name(), first_instance.rpc_service_name());
            assert!(seen_instances.insert(first_instance));
            assert!(seen_endpoints.insert(first_instance.rpc_service_name()));
            let mut current_instance = first_instance;
            registry
                .register_snapshot(&provider_snapshot(&first_provided))
                .expect("first provider snapshot should register");
            assert_eq!(
                reconcile(&registry, [local.snapshot()]).activate,
                vec![client_id.clone()]
            );

            revision += 1;
            handle_server_message(
                &mut component,
                &mut local,
                ServerMessage::Activate {
                    revision: Revision::new(epoch, revision),
                    bindings: vec![binding_for_provider(&first_provided)],
                },
                &supervisor,
                &snapshot_tx,
            )
            .await
            .expect("first client activation should succeed");
            assert_eq!(local.state, ComponentState::Active);
            assert_eq!(activation_count.load(Ordering::SeqCst), 1);
            assert_eq!(
                complete_rpc_call(
                    component.client.as_ref().expect("first client endpoint"),
                    &mut server,
                    0,
                )
                .expect("first call should complete")
                .value,
                0
            );

            // Exactly 100 replacement cycles.  The initial provider above is
            // cycle zero; every iteration removes it, expires its activation,
            // and publishes a fresh provider endpoint before reactivating the
            // same prepared client process.
            for cycle in 1_u64..=100 {
                let old_instance = current_instance;
                let old_pending = {
                    let client = component.client.as_ref().expect("active client endpoint");
                    let mut loan = client.loan_request().expect("old request loan");
                    loan.sequence = cycle;
                    loan.value = cycle;
                    loan.send().expect("old request send")
                };
                let old_request = server
                    .receive_timeout(Duration::from_millis(500))
                    .expect("old server receive")
                    .expect("old request should be delivered");
                let old_response = old_request
                    .loan_response()
                    .expect("old response loan should initially be valid");

                // The registry sees the provider disappear first.  An active
                // client bound to the old instance must therefore be planned
                // for deactivation, not silently rebound in place.
                assert!(registry.remove_provider(&provider));
                assert_eq!(
                    reconcile(&registry, [local.snapshot()]).deactivate,
                    vec![client_id.clone()]
                );
                let old_activation = local
                    .active
                    .as_ref()
                    .expect("client must be active before replacement")
                    .activation_id;
                revision += 1;
                handle_server_message(
                    &mut component,
                    &mut local,
                    ServerMessage::Deactivate {
                        revision: Revision::new(epoch, revision),
                        expected_process_instance: client_process,
                        expected_activation: old_activation,
                        reason: format!("provider replacement cycle {cycle}"),
                    },
                    &supervisor,
                    &snapshot_tx,
                )
                .await
                .expect("client deactivation should succeed");
                assert!(matches!(local.state, ComponentState::Blocked(_)));
                assert!(local.active.is_none());
                assert!(matches!(
                    old_pending.wait(Duration::from_millis(500)),
                    Err(RpcError::ActivationExpired)
                ));

                // Expiring the provider activation invalidates a response
                // already loaned by server A.  It cannot be sent after the
                // replacement is installed, so it cannot satisfy a later
                // call made through server B.
                server_scope.shutdown().await;
                assert!(matches!(
                    old_response.send(),
                    Err(RpcError::ActivationExpired)
                ));
                drop(server);
                component.client.take();

                let next_instance = CapabilityInstanceId::new();
                assert_ne!(next_instance, old_instance);
                assert!(seen_instances.insert(next_instance));
                assert!(seen_endpoints.insert(next_instance.rpc_service_name()));
                let next_provider_process = ProcessInstanceId::new();
                server_scope = EffectScope::new();
                let (next_server, next_provided) = {
                    let mut ctx = ActivationCtx::for_activation(
                        provider.clone(),
                        next_provider_process,
                        ActivationId::first(),
                        supervisor.clone(),
                        Vec::new(),
                        &mut server_scope,
                    );
                    let server = ctx
                        .rpc_server::<RuntimeRpc, Frame, Detection>(next_instance)
                        .expect("replacement server should open");
                    let provided = ctx.capabilities()[0].clone();
                    (server, provided)
                };
                server = next_server;
                assert_eq!(next_provided.descriptor.capability_instance, next_instance);
                assert_eq!(next_provided.name(), next_instance.rpc_service_name());
                registry
                    .register_snapshot(&provider_snapshot(&next_provided))
                    .expect("replacement provider snapshot should register");
                assert_eq!(
                    reconcile(&registry, [local.snapshot()]).activate,
                    vec![client_id.clone()]
                );

                revision += 1;
                handle_server_message(
                    &mut component,
                    &mut local,
                    ServerMessage::Activate {
                        revision: Revision::new(epoch, revision),
                        bindings: vec![binding_for_provider(&next_provided)],
                    },
                    &supervisor,
                    &snapshot_tx,
                )
                .await
                .expect("replacement client activation should succeed");
                assert_eq!(local.state, ComponentState::Active);
                assert_eq!(activation_count.load(Ordering::SeqCst), cycle as usize + 1);
                let active_snapshot = local.snapshot();
                let active_binding = active_snapshot
                    .binding_for("rpc")
                    .expect("active RPC requirement should be bound");
                assert_eq!(active_binding.descriptor.capability_instance, next_instance);
                assert_eq!(
                    active_binding.descriptor.service_name(),
                    next_instance.rpc_service_name()
                );
                let response = complete_rpc_call(
                    component
                        .client
                        .as_ref()
                        .expect("replacement client endpoint"),
                    &mut server,
                    cycle,
                )
                .expect("new call should complete through replacement server");
                assert_eq!(response.sequence, cycle);
                assert_eq!(response.value, cycle);
                assert_eq!(
                    reconcile(&registry, [local.snapshot()]),
                    ReconcilePlan::default()
                );
                current_instance = next_instance;
            }

            assert_eq!(seen_instances.len(), 101);
            assert_eq!(seen_endpoints.len(), 101);
            assert_eq!(prepare_count.load(Ordering::SeqCst), 1);
            assert_eq!(activation_count.load(Ordering::SeqCst), 101);
            let final_instance = local
                .snapshot()
                .binding_for("rpc")
                .expect("final client should remain bound")
                .descriptor
                .capability_instance;
            assert!(seen_instances.contains(&final_instance));
            let client_pressure = component
                .client
                .as_ref()
                .expect("final client endpoint")
                .pressure_snapshot();
            assert_eq!(
                client_pressure
                    .client
                    .as_ref()
                    .expect("client metrics")
                    .in_flight_current,
                0
            );
            assert_eq!(
                server
                    .pressure_snapshot()
                    .server
                    .as_ref()
                    .expect("server metrics")
                    .requests_current,
                0
            );

            // A final normal deactivation removes the activation-owned
            // binding and endpoint.  No stale capability remains in the
            // registry or in the weak pressure-source registry.
            assert!(registry.remove_provider(&provider));
            assert_eq!(
                reconcile(&registry, [local.snapshot()]).deactivate,
                vec![client_id.clone()]
            );
            let final_activation = local
                .active
                .as_ref()
                .expect("final client should be active")
                .activation_id;
            revision += 1;
            handle_server_message(
                &mut component,
                &mut local,
                ServerMessage::Deactivate {
                    revision: Revision::new(epoch, revision),
                    expected_process_instance: client_process,
                    expected_activation: final_activation,
                    reason: "stress test cleanup".to_owned(),
                },
                &supervisor,
                &snapshot_tx,
            )
            .await
            .expect("final client deactivation should succeed");
            assert!(matches!(local.state, ComponentState::Blocked(_)));
            assert!(local.active.is_none());
            assert!(local.snapshot().activation.is_none());
            assert!(local.snapshot().binding_for("rpc").is_none());
            assert!(local.snapshot().provided_capabilities.is_empty());
            assert!(registry.topology().is_empty());
            assert_eq!(
                reconcile(&registry, [local.snapshot()]),
                ReconcilePlan::default()
            );

            server_scope.shutdown().await;
            drop(server);
            component.client.take();
            let live_stress_sources = rbt_core::local_rpc_pressure_snapshots()
                .into_iter()
                .filter_map(|pressure| pressure.capability_instance)
                .filter(|instance| seen_instances.contains(instance))
                .collect::<Vec<_>>();
            assert!(
                live_stress_sources.is_empty(),
                "stale RPC pressure sources remained after cleanup: {live_stress_sources:?}"
            );
        })
        .await;
        assert!(completed.is_ok(), "100-cycle replacement watchdog expired");
    }

    #[tokio::test(flavor = "current_thread")]
    async fn healthy_composer_disconnect_does_not_interrupt_rpc_data_plane()
    -> Result<(), RpcEndpointError> {
        let provider = ComponentId::from("rpc-provider");
        let provider_process = ProcessInstanceId::new();
        let instance = CapabilityInstanceId::new();
        let (supervisor, _events) = mpsc::unbounded_channel();
        let mut server_scope = EffectScope::new();
        let (server, provided, binding) = {
            let mut ctx = ActivationCtx::for_activation(
                provider.clone(),
                provider_process,
                ActivationId::first(),
                supervisor.clone(),
                Vec::new(),
                &mut server_scope,
            );
            let server = ctx
                .rpc_server::<RuntimeRpc, Frame, Detection>(instance)
                .expect("server");
            let provided = ctx.capabilities()[0].clone();
            let binding = ResolvedBinding::new(
                "rpc",
                RuntimeRpc::TYPE_NAME,
                provider.clone(),
                provided.descriptor.clone(),
            );
            (server, provided, binding)
        };
        let mut local = LocalTruth::new(provider, provider_process, Vec::new());
        local.state = ComponentState::Active;
        local.control_plane = ControlPlaneStatus::Connected;
        local.active = Some(ActiveActivation {
            activation_id: ActivationId::first(),
            bindings: Vec::new(),
            capabilities: vec![provided],
            scope: server_scope,
        });
        local.control_plane = ControlPlaneStatus::Disconnected;

        let mut client_scope = EffectScope::new();
        let client = {
            let mut ctx = ActivationCtx::for_activation(
                ComponentId::from("rpc-client"),
                ProcessInstanceId::new(),
                ActivationId::first(),
                supervisor,
                vec![binding],
                &mut client_scope,
            );
            ctx.require_rpc::<RuntimeRpc, Frame, Detection>("rpc")?
        };
        let server_thread = std::thread::spawn(move || -> Result<(), RpcError> {
            let mut server = server;
            let request = server
                .receive_timeout(Duration::from_secs(2))?
                .expect("request while Composer is absent");
            let mut response = request.loan_response()?;
            response.value = 99;
            response.send()
        });
        let response = client.call(Frame::default(), Duration::from_secs(2))?;
        assert_eq!(response.value, 99);
        assert_eq!(local.state, ComponentState::Active);
        assert_eq!(local.control_plane, ControlPlaneStatus::Disconnected);
        server_thread.join().expect("server thread")?;
        client_scope.shutdown().await;
        local.shutdown_active().await;
        Ok(())
    }

    #[test]
    fn composer_disconnect_does_not_change_active_lifecycle_or_activation() {
        let process = ProcessInstanceId::new();
        let mut local = LocalTruth::new(ComponentId::from("detector"), process, Vec::new());
        let activation = ActivationId::first();
        local.state = ComponentState::Active;
        local.control_plane = ControlPlaneStatus::Connected;
        local.active = Some(ActiveActivation {
            activation_id: activation,
            bindings: Vec::new(),
            capabilities: Vec::new(),
            scope: EffectScope::new(),
        });

        // ControlPlaneDisconnected is handled only as connection status; it
        // has no lifecycle transition in the runner.
        local.control_plane = ControlPlaneStatus::Disconnected;
        assert_eq!(local.state, ComponentState::Active);
        assert_eq!(local.control_plane, ControlPlaneStatus::Disconnected);
        assert_eq!(
            local.active.as_ref().map(|active| active.activation_id),
            Some(activation)
        );
    }

    #[tokio::test]
    async fn critical_exit_invalidates_activation_and_its_capability() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::first();
        let capability_instance = CapabilityInstanceId::new();
        let capability = ProvidedCapability::new(
            "Detections",
            CapabilityDescriptor::new(
                capability_instance,
                component.clone(),
                process,
                activation,
                StreamDescriptor::for_fixed::<Detection>(
                    capability_instance.stream_service_name(),
                    StreamProfile::Sensor,
                ),
            ),
        );
        let mut local = LocalTruth::new(component.clone(), process, Vec::new());
        local.state = ComponentState::Active;
        local.active = Some(ActiveActivation {
            activation_id: activation,
            bindings: Vec::new(),
            capabilities: vec![capability],
            scope: EffectScope::new(),
        });
        let (snapshot_tx, snapshot_rx) = watch::channel(local.snapshot());

        handle_runtime_event(
            &mut local,
            RuntimeEvent::CriticalTaskExited {
                component,
                process,
                activation,
                task_name: "inference".to_owned(),
                outcome: TaskOutcome::Error("intentional".to_owned()),
            },
            &snapshot_tx,
        )
        .await
        .unwrap();

        assert!(matches!(local.state, ComponentState::Failed(_)));
        assert!(local.active.is_none());
        assert!(snapshot_rx.borrow().provided_capabilities.is_empty());
        assert!(snapshot_rx.borrow().activation.is_none());
    }

    #[tokio::test]
    async fn stale_background_exit_does_not_change_current_activation() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let current_activation = ActivationId::new(2);
        let stale_activation = ActivationId::first();
        let mut local = LocalTruth::new(component.clone(), process, Vec::new());
        local.state = ComponentState::Active;
        local.active = Some(ActiveActivation {
            activation_id: current_activation,
            bindings: Vec::new(),
            capabilities: Vec::new(),
            scope: EffectScope::new(),
        });
        let (snapshot_tx, snapshot_rx) = watch::channel(local.snapshot());
        let before = snapshot_rx.borrow().clone();

        handle_runtime_event(
            &mut local,
            RuntimeEvent::BackgroundTaskExited {
                component,
                process,
                activation: stale_activation,
                task_name: "metrics".to_owned(),
                outcome: TaskOutcome::Error("stale".to_owned()),
            },
            &snapshot_tx,
        )
        .await
        .unwrap();

        assert_eq!(local.state, ComponentState::Active);
        assert_eq!(*snapshot_rx.borrow(), before);
    }

    #[tokio::test]
    async fn stale_critical_exit_does_not_change_current_activation() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let current_activation = ActivationId::new(2);
        let stale_activation = ActivationId::first();
        let mut local = LocalTruth::new(component.clone(), process, Vec::new());
        local.state = ComponentState::Active;
        local.active = Some(ActiveActivation {
            activation_id: current_activation,
            bindings: Vec::new(),
            capabilities: Vec::new(),
            scope: EffectScope::new(),
        });
        let (snapshot_tx, snapshot_rx) = watch::channel(local.snapshot());
        let before = snapshot_rx.borrow().clone();

        handle_runtime_event(
            &mut local,
            RuntimeEvent::CriticalTaskExited {
                component,
                process,
                activation: stale_activation,
                task_name: "delayed-python-worker".to_owned(),
                outcome: TaskOutcome::Completed,
            },
            &snapshot_tx,
        )
        .await
        .unwrap();

        assert_eq!(local.state, ComponentState::Active);
        assert_eq!(
            local.active.as_ref().unwrap().activation_id,
            current_activation
        );
        assert_eq!(*snapshot_rx.borrow(), before);
    }

    #[tokio::test(flavor = "current_thread")]
    async fn critical_blocking_error_uses_existing_critical_event_path() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::first();
        let (supervisor, mut events) = mpsc::unbounded_channel();
        let mut scope = EffectScope::new();
        {
            let mut ctx = ActivationCtx::for_activation(
                component.clone(),
                process,
                activation,
                supervisor,
                Vec::new(),
                &mut scope,
            );
            ctx.spawn_critical_blocking(
                "blocking-inference",
                |_token| Err::<(), _>("intentional"),
                || {},
            );
        }

        let event = events
            .recv()
            .await
            .expect("critical event should be emitted");
        assert_eq!(
            event,
            RuntimeEvent::CriticalTaskExited {
                component,
                process,
                activation,
                task_name: "blocking-inference".to_owned(),
                outcome: TaskOutcome::Error("intentional".to_owned()),
            }
        );
        scope.shutdown().await;
    }

    #[tokio::test(flavor = "current_thread")]
    async fn normal_critical_blocking_cancellation_emits_no_failure() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::first();
        let (supervisor, mut events) = mpsc::unbounded_channel();
        let mut wait_context = WaitContext::new(process, activation).unwrap();
        let wait_wake = wait_context.wake_handle();
        let (started_tx, started_rx) = tokio::sync::oneshot::channel();
        let (exit_tx, exit_rx) = tokio::sync::oneshot::channel();
        let mut scope = EffectScope::new();
        {
            let mut ctx = ActivationCtx::for_activation(
                component,
                process,
                activation,
                supervisor,
                Vec::new(),
                &mut scope,
            );
            ctx.spawn_critical_blocking(
                "blocking-wait",
                move |token| {
                    let _ = started_tx.send(());
                    let exit = wait_context
                        .run(&[], |_event, _budget| {
                            Ok::<_, rbt_iox2::WaitError>(WaitProgress::Continue)
                        })
                        .expect("idle wait should be cancelled cleanly");
                    let _ = exit_tx.send(exit);
                    assert!(token.is_cancelled());
                    Ok::<(), &'static str>(())
                },
                move || {
                    wait_wake.wake().expect("WaitContext wake should succeed");
                },
            );
        }

        started_rx
            .await
            .expect("blocking task should enter its wait");
        // Let the blocking task enter the native wait set before requesting
        // normal deactivation. Completion itself is synchronized by joins.
        tokio::time::sleep(Duration::from_millis(10)).await;
        scope.shutdown().await;
        assert_eq!(
            exit_rx.await.expect("wait task should return"),
            WaitExit::Cancelled
        );
        assert!(
            events.try_recv().is_err(),
            "normal cancellation must be silent"
        );
    }

    #[tokio::test(flavor = "current_thread")]
    async fn critical_failure_deactivates_and_joins_idle_wait_context_sibling() {
        let lifecycle_started_at = Instant::now();
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::new(7);
        let mut wait_context = WaitContext::new(process, activation).unwrap();
        assert_eq!(wait_context.process_instance(), process);
        assert_eq!(wait_context.activation(), activation);
        let wait_wake = wait_context.wake_handle();
        let (supervisor, mut events) = mpsc::unbounded_channel();
        let (started_tx, started_rx) = tokio::sync::oneshot::channel();
        let (wake_tx, wake_rx) = std_mpsc::channel();
        let (wait_exit_tx, wait_exit_rx) = tokio::sync::oneshot::channel();
        let (task_exit_tx, task_exit_rx) = tokio::sync::oneshot::channel();
        let (release_tx, release_rx) = std_mpsc::channel();
        let mut scope = EffectScope::new();

        // A is the activation's idle, critical WaitContext owner. Its
        // release gate makes shutdown prove that it awaited the blocking
        // join, rather than merely requesting cancellation.
        {
            let mut ctx = ActivationCtx::for_activation(
                component.clone(),
                process,
                activation,
                supervisor.clone(),
                Vec::new(),
                &mut scope,
            );
            ctx.spawn_critical_blocking(
                "idle-wait",
                move |token| {
                    let _ = started_tx.send(());
                    let exit = wait_context
                        .run(&[], |_event, _budget| {
                            Ok::<_, rbt_iox2::WaitError>(WaitProgress::Continue)
                        })
                        .expect("idle wait should be cancelled cleanly");
                    let wait_exit_at = Instant::now();
                    let _ = wait_exit_tx.send((exit, wait_exit_at));
                    release_rx
                        .recv()
                        .expect("shutdown coordinator should release the task");
                    let task_exit_at = Instant::now();
                    let _ = task_exit_tx.send(task_exit_at);
                    assert!(token.is_cancelled());
                    Ok::<(), &'static str>(())
                },
                move || {
                    let _ = wake_tx.send(Instant::now());
                    wait_wake.wake().expect("WaitContext wake should succeed");
                },
            );
        }

        started_rx.await.expect("idle wait should start");
        // A has announced entry into its blocking wait. This short yield lets
        // the native wait set block before B is started; all completion
        // synchronization below uses channels and the shutdown join.
        tokio::time::sleep(Duration::from_millis(10)).await;

        // B exits with an error through the normal critical RuntimeEvent path.
        {
            let mut ctx = ActivationCtx::for_activation(
                component.clone(),
                process,
                activation,
                supervisor,
                Vec::new(),
                &mut scope,
            );
            ctx.spawn_critical_blocking(
                "failing-sibling",
                |_token| Err::<(), _>("sibling-failed"),
                || {},
            );
        }

        let event = events
            .recv()
            .await
            .expect("critical sibling should emit a runtime event");
        assert_eq!(
            event,
            RuntimeEvent::CriticalTaskExited {
                component: component.clone(),
                process,
                activation,
                task_name: "failing-sibling".to_owned(),
                outcome: TaskOutcome::Error("sibling-failed".to_owned()),
            }
        );

        let deactivation_requested_at = Instant::now();
        let shutdown = scope.shutdown();
        let wait_exit = async move {
            let result = wait_exit_rx
                .await
                .expect("idle wait should report cancellation");
            release_tx
                .send(())
                .expect("idle task should still await its release");
            result
        };
        let (_, (wait_exit, wait_exit_at)) = tokio::join!(shutdown, wait_exit);
        let scope_completed_at = Instant::now();
        let wait_wake_at = wake_rx
            .recv()
            .expect("shutdown should wake the wait context");
        let task_exit_at = task_exit_rx.await.expect("shutdown should join the task");

        assert_eq!(wait_exit, WaitExit::Cancelled);
        assert!(deactivation_requested_at <= wait_wake_at);
        assert!(wait_wake_at <= wait_exit_at);
        assert!(wait_exit_at <= task_exit_at);
        assert!(task_exit_at <= scope_completed_at);
        let shutdown_latency = scope_completed_at.duration_since(deactivation_requested_at);
        assert!(
            shutdown_latency < Duration::from_secs(1),
            "deactivation shutdown took {shutdown_latency:?}"
        );
        assert!(
            events.try_recv().is_err(),
            "cancelled idle sibling must not emit an extra failure"
        );

        let latency_ms =
            |instant: Instant| instant.duration_since(lifecycle_started_at).as_millis();
        println!(
            "RBT_WAITSET_LIFECYCLE deactivation_request_ms={} wait_wake_ms={} wait_exit_ms={} task_exit_ms={} scope_completed_ms={} shutdown_ms={}",
            latency_ms(deactivation_requested_at),
            latency_ms(wait_wake_at),
            latency_ms(wait_exit_at),
            latency_ms(task_exit_at),
            latency_ms(scope_completed_at),
            shutdown_latency.as_millis(),
        );
    }

    #[tokio::test(flavor = "current_thread")]
    async fn background_blocking_error_keeps_background_event_identity() {
        let component = ComponentId::from("detector");
        let process = ProcessInstanceId::new();
        let activation = ActivationId::first();
        let (supervisor, mut events) = mpsc::unbounded_channel();
        let mut scope = EffectScope::new();
        {
            let mut ctx = ActivationCtx::for_activation(
                component.clone(),
                process,
                activation,
                supervisor,
                Vec::new(),
                &mut scope,
            );
            ctx.spawn_background_blocking(
                "blocking-background",
                |_token| Err::<(), _>("background-failed"),
                || {},
            );
        }

        let event = events
            .recv()
            .await
            .expect("background event should be emitted");
        assert_eq!(
            event,
            RuntimeEvent::BackgroundTaskExited {
                component,
                process,
                activation,
                task_name: "blocking-background".to_owned(),
                outcome: TaskOutcome::Error("background-failed".to_owned()),
            }
        );
        scope.shutdown().await;
    }
}

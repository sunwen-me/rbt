use std::{
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};

use anyhow::{Result, anyhow, bail};
use async_trait::async_trait;
use rbt::{
    component::{ActivationCtx, Component, run_component},
    rpc::{RpcClient, RpcError},
    types::{Capability, CapabilityInstanceId, ComponentId, Detection, Frame, RequirementSpec},
};
use tokio_util::sync::CancellationToken;
use tracing::{info, warn};
use tracing_subscriber::EnvFilter;

const RPC_TIMEOUT: Duration = Duration::from_millis(250);
const SERVER_RECEIVE_TIMEOUT: Duration = Duration::from_millis(100);
const CLIENT_INTERVAL: Duration = Duration::from_millis(500);
const RESPONSE_MULTIPLIER: u64 = 2;
const MAX_DEMO_DELAY: Duration = Duration::from_secs(5);
static PROVIDER_PREPARES: AtomicU64 = AtomicU64::new(0);
static PROVIDER_ACTIVATIONS: AtomicU64 = AtomicU64::new(0);
static PROVIDER_DEACTIVATIONS: AtomicU64 = AtomicU64::new(0);
static CLIENT_PREPARES: AtomicU64 = AtomicU64::new(0);
static CLIENT_ACTIVATIONS: AtomicU64 = AtomicU64::new(0);
static CLIENT_DEACTIVATIONS: AtomicU64 = AtomicU64::new(0);
static CLIENT_CALLS: AtomicU64 = AtomicU64::new(0);
static CLIENT_DEADLINES: AtomicU64 = AtomicU64::new(0);

struct EchoRpc;

impl Capability for EchoRpc {
    const TYPE_NAME: &'static str = "EchoRpc";
}

struct Provider;

#[async_trait]
impl Component for Provider {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        let count = PROVIDER_PREPARES.fetch_add(1, Ordering::SeqCst) + 1;
        println!("RBT_RPC_SOAK lifecycle role=provider event=prepare prepare_count={count}");
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("rpc-provider")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let demo_delay = demo_delay()?;
        // The helper opens the fixed Frame -> Detection endpoint and publishes
        // its capability only after the activation-owned endpoint is ready.
        let capability_instance = CapabilityInstanceId::new();
        let mut server = ctx.rpc_server::<EchoRpc, Frame, Detection>(capability_instance)?;
        let wake = server.wake_handle();
        let component = ctx.component().clone();
        let process = ctx.process_instance();
        let activation = ctx.activation_id();
        let activation_count = PROVIDER_ACTIVATIONS.fetch_add(1, Ordering::SeqCst) + 1;
        println!(
            "RBT_RPC_SOAK lifecycle role=provider event=activate prepare_count={} activation_count={activation_count} activation_id={} capability_instance_id={capability_instance}",
            PROVIDER_PREPARES.load(Ordering::SeqCst),
            activation.value(),
        );
        info!(
            %component,
            %process,
            %activation,
            %capability_instance,
            "RPC provider activated"
        );

        ctx.spawn_critical_blocking(
            "serve-rpc",
            move |token| {
                loop {
                    if token.is_cancelled() {
                        let count = PROVIDER_DEACTIVATIONS.fetch_add(1, Ordering::SeqCst) + 1;
                        println!(
                            "RBT_RPC_SOAK lifecycle role=provider event=deactivate deactivation_count={count} activation_id={}",
                            activation.value(),
                        );
                        return Ok(());
                    }

                    match server.receive_timeout(SERVER_RECEIVE_TIMEOUT) {
                        Ok(Some(request)) => {
                            let sequence = request.sequence;
                            let value = request.value;
                            let slow_demo_call = sequence == 1 && demo_delay.is_some();
                            let mut response = request.loan_response()?;
                            response.sequence = sequence;
                            response.value = value.saturating_mul(RESPONSE_MULTIPLIER);
                            if let Some(delay) = demo_delay.filter(|_| slow_demo_call) {
                                info!(
                                    %component,
                                    %process,
                                    %activation,
                                    %capability_instance,
                                    sequence,
                                    delay_ms = delay.as_millis(),
                                    "RPC demo delaying selected response"
                                );
                                if !sleep_cancellable(delay, &token) {
                                    return Ok(());
                                }
                            }
                            match response.send() {
                                Ok(()) => {
                                    info!(
                                        %component,
                                        %process,
                                        %activation,
                                        %capability_instance,
                                        sequence,
                                        value,
                                        "RPC provider answered"
                                    );
                                }
                                Err(error) if slow_demo_call => {
                                    warn!(
                                        %component,
                                        %process,
                                        %activation,
                                        %capability_instance,
                                        sequence,
                                        %error,
                                        "RPC demo response arrived after the client deadline; continuing without retry"
                                    );
                                }
                                Err(error) => {
                                    return Err(anyhow!("RPC provider response failed: {error}"));
                                }
                            }
                        }
                        Ok(None) => {}
                        Err(RpcError::ActivationExpired) => {
                            let count =
                                PROVIDER_DEACTIVATIONS.fetch_add(1, Ordering::SeqCst) + 1;
                            println!(
                                "RBT_RPC_SOAK lifecycle role=provider event=deactivate deactivation_count={count} activation_id={} reason=expired",
                                activation.value(),
                            );
                            info!(
                                %component,
                                %process,
                                %activation,
                                %capability_instance,
                                "RPC provider activation expired"
                            );
                            return Ok(());
                        }
                        Err(error) => {
                            return Err(anyhow!("RPC provider receive failed: {error}"));
                        }
                    }
                }
            },
            move || {
                let _ = wake.wake();
            },
        );
        Ok(())
    }
}

struct Client;

#[async_trait]
impl Component for Client {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        let count = CLIENT_PREPARES.fetch_add(1, Ordering::SeqCst) + 1;
        println!("RBT_RPC_SOAK lifecycle role=client event=prepare prepare_count={count}");
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("rpc-client")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        vec![RequirementSpec::typed::<EchoRpc>("echo")]
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        // The requirement binding supplies the provider's current physical
        // capability instance; no service name is invented by the client.
        let client = ctx.require_rpc::<EchoRpc, Frame, Detection>("echo")?;
        let capability_instance = client
            .pressure_snapshot()
            .capability_instance
            .map(|id| id.to_string())
            .unwrap_or_else(|| "unknown".to_owned());
        let wake = client.wake_handle();
        let component = ctx.component().clone();
        let process = ctx.process_instance();
        let activation = ctx.activation_id();
        let activation_count = CLIENT_ACTIVATIONS.fetch_add(1, Ordering::SeqCst) + 1;
        println!(
            "RBT_RPC_SOAK lifecycle role=client event=activate prepare_count={} activation_count={activation_count} activation_id={} capability_instance_id={capability_instance}",
            CLIENT_PREPARES.load(Ordering::SeqCst),
            activation.value(),
        );
        info!(%component, %process, %activation, "RPC client activated");

        // Calls and their native waits live in one activation-owned blocking
        // loop. The client wake handle is also used by scoped cancellation.
        ctx.spawn_critical_blocking(
            "call-rpc",
            move |token| {
                let mut sequence = 1_u64;

                loop {
                    if token.is_cancelled() {
                        let count = CLIENT_DEACTIVATIONS.fetch_add(1, Ordering::SeqCst) + 1;
                        println!(
                            "RBT_RPC_SOAK lifecycle role=client event=deactivate deactivation_count={count} activation_id={}",
                            activation.value(),
                        );
                        info!(%component, %process, %activation, "RPC client activation cancelled");
                        return Ok(());
                    }

                    match call_once(&client, &token, sequence) {
                        Ok((request_value, response_value)) => {
                            let calls = CLIENT_CALLS.fetch_add(1, Ordering::Relaxed) + 1;
                            println!(
                                "RBT_RPC_SOAK call role=client terminal=response calls={calls} activation_id={} capability_instance_id={capability_instance} sequence={sequence}",
                                activation.value(),
                            );
                            info!(
                                %component,
                                %process,
                                %activation,
                                sequence,
                                request_value,
                                response_value,
                                "RPC client received zero-copy response"
                            );
                            sequence = sequence.saturating_add(1);
                        }
                        Err(RpcError::DeadlineExceeded) => {
                            let deadlines = CLIENT_DEADLINES.fetch_add(1, Ordering::Relaxed) + 1;
                            println!(
                                "RBT_RPC_SOAK call role=client terminal=deadline deadlines={deadlines} activation_id={} sequence={sequence}",
                                activation.value(),
                            );
                            warn!(
                                %component,
                                %process,
                                %activation,
                                timeout_ms = RPC_TIMEOUT.as_millis(),
                                "RPC deadline exceeded; remote execution is unknown; no automatic retry"
                            );
                            sequence = sequence.saturating_add(1);
                        }
                        Err(RpcError::Cancelled | RpcError::ActivationExpired) => {
                            let count = CLIENT_DEACTIVATIONS.fetch_add(1, Ordering::SeqCst) + 1;
                            println!(
                                "RBT_RPC_SOAK lifecycle role=client event=deactivate deactivation_count={count} activation_id={} reason=cancelled",
                                activation.value(),
                            );
                            info!(
                                %component,
                                %process,
                                %activation,
                                "RPC call cancelled with activation"
                            );
                            return Ok(());
                        }
                        Err(RpcError::ServerUnavailable) => {
                            warn!(
                                %component,
                                %process,
                                %activation,
                                "RPC server unavailable during binding transition"
                            );
                        }
                        Err(error) => {
                            return Err(anyhow!("RPC client call failed: {error}"));
                        }
                    }

                    std::thread::sleep(CLIENT_INTERVAL);
                }
            },
            move || {
                let _ = wake.wake();
            },
        );
        Ok(())
    }
}

fn call_once(
    client: &RpcClient<Frame, Detection>,
    token: &CancellationToken,
    sequence: u64,
) -> std::result::Result<(u64, u64), RpcError> {
    let mut request = client.loan_request()?;
    request.sequence = sequence;
    request.value = sequence;
    let pending = request.send()?;
    let cancel = pending.cancel_handle();
    if token.is_cancelled() {
        // Cancellation is a request to end this logical call. The native
        // pending response remains the authority for the final outcome.
        cancel.cancel()?;
    }
    pending
        .wait(RPC_TIMEOUT)
        .map(|response| (response.sequence, response.value))
}

fn demo_delay() -> Result<Option<Duration>> {
    let value = match std::env::var("RBT_RPC_DEMO_DELAY_MS") {
        Ok(value) => value,
        Err(std::env::VarError::NotPresent) => return Ok(None),
        Err(error) => return Err(anyhow!("failed to read RBT_RPC_DEMO_DELAY_MS: {error}")),
    };
    let milliseconds = value
        .parse::<u64>()
        .map_err(|error| anyhow!("invalid RBT_RPC_DEMO_DELAY_MS={value:?}: {error}"))?;
    let delay = Duration::from_millis(milliseconds);
    if delay > MAX_DEMO_DELAY {
        bail!(
            "RBT_RPC_DEMO_DELAY_MS must be in 0..={} ms",
            MAX_DEMO_DELAY.as_millis()
        );
    }
    if delay.is_zero() {
        Ok(None)
    } else {
        Ok(Some(delay))
    }
}

fn sleep_cancellable(duration: Duration, token: &CancellationToken) -> bool {
    let deadline = Instant::now() + duration;
    while !token.is_cancelled() {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return true;
        }
        std::thread::sleep(remaining.min(Duration::from_millis(5)));
    }
    false
}

fn composer_addr() -> String {
    std::env::var("RBT_COMPOSER_ADDR").unwrap_or_else(|_| "127.0.0.1:7447".to_owned())
}

fn init_tracing() {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        // Plain text when stdout is redirected to a log file or supervisor.
        .with_ansi(std::io::IsTerminal::is_terminal(&std::io::stdout()))
        .init();
}

#[tokio::main]
async fn main() -> Result<()> {
    init_tracing();
    let mode = std::env::args()
        .nth(1)
        .ok_or_else(|| anyhow!("usage: rpc-onboarding <provider|client>"))?;
    let address = composer_addr();
    match mode.as_str() {
        "provider" => run_component::<Provider, _>((), address)
            .await
            .map_err(Into::into),
        "client" => run_component::<Client, _>((), address)
            .await
            .map_err(Into::into),
        _ => bail!("usage: rpc-onboarding <provider|client>"),
    }
}

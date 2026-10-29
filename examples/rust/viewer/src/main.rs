use std::time::{Duration, Instant};

use anyhow::Result;
use async_trait::async_trait;
use rbt::{
    component::{ActivationCtx, Component, run_component},
    stream::{Stream, WaitContext, WaitExit, WaitProgress, WaitToken},
    types::{Capability, ComponentId, Detection, RequirementSpec},
};
use tracing::{debug, error, info};
use tracing_subscriber::EnvFilter;

struct Detections;

impl Capability for Detections {
    const TYPE_NAME: &'static str = "Detections";
}

struct Viewer;

#[async_trait]
impl Component for Viewer {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("viewer")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        vec![RequirementSpec::typed::<Detections>("detections")]
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let binding = ctx.binding::<Detections>("detections")?;
        let capability = binding.descriptor.capability_instance;
        let provider = binding.provider.to_string();
        let component = ctx.component().clone();
        let process = ctx.process_instance();
        let activation = ctx.activation_id();
        let subscriber = Stream::<Detection>::subscriber(binding.stream())?;
        let wait_context = WaitContext::new(process, activation)
            .map_err(|error| anyhow::anyhow!("viewer failed to create wait context: {error}"))?;
        let wake = wait_context.wake_handle();
        info!(
            component = %component,
            process_instance = %process,
            activation = %activation,
            capability_instance = %capability,
            provider = %provider,
            "viewer activated"
        );

        ctx.spawn_critical_blocking(
            "receive",
            move |token| {
                let mut context = wait_context;
                let subscriber = subscriber;
                let mut last_showcase: Option<Instant> = None;
                let result = {
                    let source = subscriber.event_source(WaitToken::new(1));
                    context.run(std::slice::from_ref(&source), |event, budget| {
                if event.is_cancellation() || token.is_cancelled() {
                    return Ok(WaitProgress::continue_waiting());
                }

                let mut drained = 0_usize;
                for _ in 0..budget {
                    if token.is_cancelled() {
                        break;
                    }

                    match subscriber.receive() {
                        Ok(Some(detection)) => {
                            debug!(
                                component = %component,
                                process_instance = %process,
                                activation = %activation,
                                capability_instance = %capability,
                                provider = %provider,
                                sequence = detection.sequence,
                                value = detection.value,
                                "received detection"
                            );
                            let show = last_showcase
                                .map(|instant| instant.elapsed() >= Duration::from_secs(1))
                                .unwrap_or(true);
                            if show {
                                println!(
                                    "VIEW component={} process_instance={} activation={} capability_instance={} provider={} detection seq={} value={}",
                                    component,
                                    process,
                                    activation,
                                    capability,
                                    provider,
                                    detection.sequence,
                                    detection.value
                                );
                                last_showcase = Some(Instant::now());
                            }
                            drained = drained.saturating_add(1);
                        }
                        Ok(None) => {
                            return Ok(if drained == 0 {
                                WaitProgress::empty()
                            } else {
                                WaitProgress::samples(drained)
                            });
                        }
                        Err(error) => {
                            error!(
                                component = %component,
                                process_instance = %process,
                                activation = %activation,
                                capability_instance = %capability,
                                provider = %provider,
                                %error,
                                "viewer failed to receive detection"
                            );
                            return Err(anyhow::anyhow!(
                                "viewer failed to receive detection: {error}"
                            ));
                        }
                    }
                }

                if drained == 0 {
                    Ok(WaitProgress::continue_waiting())
                } else {
                    Ok(WaitProgress::samples(drained))
                }
                    })
                };

                match result {
                Ok(WaitExit::Cancelled | WaitExit::Stopped) => Ok(()),
                Err(error) => {
                    error!(
                        component = %component,
                        process_instance = %process,
                        activation = %activation,
                        capability_instance = %capability,
                        provider = %provider,
                        %error,
                        "viewer wait loop failed"
                    );
                    Err(error)
                }
            }
            },
            move || {
                if let Err(error) = wake.wake() {
                    error!(%error, "viewer wait cancellation wake failed");
                }
            },
        );

        Ok(())
    }
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

fn composer_addr() -> String {
    std::env::var("RBT_COMPOSER_ADDR").unwrap_or_else(|_| "127.0.0.1:7447".to_owned())
}

#[tokio::main]
async fn main() -> std::result::Result<(), rbt::ControlPlaneError> {
    init_tracing();
    run_component::<Viewer, _>((), composer_addr()).await
}

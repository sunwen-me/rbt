use std::{
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};

use anyhow::{Result, anyhow};
use async_trait::async_trait;
use rbt::{
    component::{ActivationCtx, Component, run_component},
    stream::{SliceStream, Stream, WaitContext, WaitExit, WaitProgress, WaitToken},
    types::{
        Capability, CapabilityInstanceId, ComponentId, Detection, Image, RequirementSpec,
        StreamProfile,
    },
};
use tracing::{debug, info};
use tracing_subscriber::EnvFilter;

mod test_faults;

use test_faults::TestFaults;

static NEXT_MODEL_ID: AtomicU64 = AtomicU64::new(1);

struct Camera;

impl Capability for Camera {
    const TYPE_NAME: &'static str = "Camera";
}

struct Detections;

impl Capability for Detections {
    const TYPE_NAME: &'static str = "Detections";
}

struct FakeModel {
    load_id: u64,
}

struct Detector {
    model: FakeModel,
    test_faults: TestFaults,
}

#[async_trait]
impl Component for Detector {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        let model = FakeModel {
            load_id: NEXT_MODEL_ID.fetch_add(1, Ordering::Relaxed),
        };
        println!("MODEL LOADED id={}", model.load_id);
        Ok(Self {
            model,
            test_faults: TestFaults::from_env()?,
        })
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("detector")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        vec![RequirementSpec::typed::<Camera>("camera")]
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let binding = ctx.binding::<Camera>("camera")?;
        let subscriber = SliceStream::<Image>::subscriber(binding.stream())?;
        let provider = binding.provider.to_string();
        let input_capability = binding.descriptor.capability_instance;

        let process = ctx.process_instance();
        let activation = ctx.activation_id();
        let output_capability = CapabilityInstanceId::new();
        let output_service = output_capability.stream_service_name();
        let output_descriptor =
            Stream::<Detection>::descriptor(output_service, StreamProfile::Sensor);
        let publisher = Stream::<Detection>::publisher(&output_descriptor)?;
        ctx.provide::<Detections>(output_capability, output_descriptor);
        let wait_context = WaitContext::new(process, activation)
            .map_err(|error| anyhow!("detector failed to create wait context: {error}"))?;
        let wake = wait_context.wake_handle();

        let model_id = self.model.load_id;
        let test_faults = self.test_faults;
        let component = ctx.component().clone();
        info!(
            component = %component,
            process_instance = %process,
            activation = %activation,
            provider = %provider,
            capability_instance = %output_capability,
            input_capability_instance = %input_capability,
            model_id,
            fail_after = ?test_faults.fail_after,
            exit_after = ?test_faults.exit_after,
            pause_ms = ?test_faults.pause_ms,
            "detector activated"
        );

        ctx.spawn_critical_blocking(
            "inference",
            move |token| {
                let mut context = wait_context;
                let subscriber = subscriber;
                let publisher = publisher;
                if test_faults.fail_after == Some(0) {
                    let failure_word = test_faults.failure_word();
                    tracing::error!(
                        component = %component,
                        process_instance = %process,
                        activation = %activation,
                        capability_instance = %output_capability,
                        input_capability_instance = %input_capability,
                        provider = %provider,
                        model_id,
                        "test {failure_word} requested before frame processing"
                    );
                    return Err(anyhow!("test {failure_word} after processing 0 frames"));
                }
                if test_faults.exit_after == Some(0) {
                    tracing::error!(
                        component = %component,
                        process_instance = %process,
                        activation = %activation,
                        capability_instance = %output_capability,
                        input_capability_instance = %input_capability,
                        provider = %provider,
                        model_id,
                        "test process exit requested before frame processing"
                    );
                    std::process::exit(1);
                }
                let mut processed = 0_u64;
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

                        let image = match subscriber.receive() {
                            Ok(Some(image)) => image,
                            Ok(None) => {
                                return Ok(if drained == 0 {
                                    WaitProgress::empty()
                                } else {
                                    WaitProgress::samples(drained)
                                });
                            }
                            Err(error) => {
                                tracing::error!(
                                    component = %component,
                                    process_instance = %process,
                                    activation = %activation,
                                    capability_instance = %output_capability,
                                    input_capability_instance = %input_capability,
                                    provider = %provider,
                                    model_id,
                                    %error,
                                    "detector failed to receive image"
                                );
                                return Err(anyhow!("detector failed to receive image: {error}"));
                            }
                        };

                        if let Some(pause_ms) =
                            test_faults.pause_ms.filter(|pause_ms| *pause_ms > 0)
                        {
                            let pause = Duration::from_millis(pause_ms);
                            let started = Instant::now();
                            while started.elapsed() < pause {
                                if token.is_cancelled() {
                                    break;
                                }
                                let remaining = pause.saturating_sub(started.elapsed());
                                std::thread::sleep(remaining.min(Duration::from_millis(1)));
                            }
                            if token.is_cancelled() {
                                break;
                            }
                        }

                        let sequence = image.header().sequence;
                        let value = image.payload().first().copied().unwrap_or(0) as u64 * 10;
                        drop(image);

                        let mut detection = match publisher.loan() {
                            Ok(detection) => detection,
                            Err(error) => {
                                tracing::error!(
                                    component = %component,
                                    process_instance = %process,
                                    activation = %activation,
                                    capability_instance = %output_capability,
                                    input_capability_instance = %input_capability,
                                    provider = %provider,
                                    model_id,
                                    %error,
                                    "detector failed to loan detection"
                                );
                                return Err(anyhow!("detector failed to loan detection: {error}"));
                            }
                        };
                        detection.sequence = sequence;
                        detection.value = value;
                        match detection.send() {
                            Ok(_) => debug!(
                                component = %component,
                                process_instance = %process,
                                activation = %activation,
                                capability_instance = %output_capability,
                                provider = %provider,
                                model_id,
                                sequence,
                                value,
                                "published detection"
                            ),
                            Err(error) => {
                                tracing::error!(
                                    component = %component,
                                    process_instance = %process,
                                    activation = %activation,
                                    capability_instance = %output_capability,
                                    input_capability_instance = %input_capability,
                                    provider = %provider,
                                    model_id,
                                    sequence,
                                    %error,
                                    "detector failed to publish detection"
                                );
                                return Err(anyhow!(
                                    "detector failed to publish detection: {error}"
                                ));
                            }
                        }

                        drained = drained.saturating_add(1);
                        processed = processed.saturating_add(1);
                        if test_faults
                            .exit_after
                            .is_some_and(|limit| processed >= limit)
                        {
                            tracing::error!(
                                component = %component,
                                process_instance = %process,
                                activation = %activation,
                                capability_instance = %output_capability,
                                input_capability_instance = %input_capability,
                                provider = %provider,
                                model_id,
                                processed,
                                "test process exit requested after successful frame processing"
                            );
                            std::process::exit(1);
                        }
                        if test_faults
                            .fail_after
                            .is_some_and(|limit| processed >= limit)
                        {
                            let failure_word = test_faults.failure_word();
                            tracing::error!(
                                component = %component,
                                process_instance = %process,
                                activation = %activation,
                                capability_instance = %output_capability,
                                input_capability_instance = %input_capability,
                                provider = %provider,
                                model_id,
                                processed,
                                "test {failure_word} requested after successful frame processing"
                            );
                            return Err(anyhow!(
                                "test {failure_word} after processing {processed} frames"
                            ));
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
                        tracing::error!(
                            component = %component,
                            process_instance = %process,
                            activation = %activation,
                            capability_instance = %output_capability,
                            input_capability_instance = %input_capability,
                            provider = %provider,
                            model_id,
                            %error,
                            "detector wait loop failed"
                        );
                        Err(error)
                    }
                }
            },
            move || {
                if let Err(error) = wake.wake() {
                    tracing::error!(%error, "detector wait cancellation wake failed");
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
    run_component::<Detector, _>((), composer_addr()).await
}

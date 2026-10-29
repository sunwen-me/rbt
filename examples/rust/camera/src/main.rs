use std::time::Duration;

use anyhow::{Result, anyhow};
use async_trait::async_trait;
use rbt::{
    component::{ActivationCtx, Component, run_component},
    stream::SliceStream,
    types::{
        Capability, CapabilityInstanceId, ComponentId, Image, PixelFormat, RequirementSpec,
        StreamProfile,
    },
};
use tracing::{debug, error, info};
use tracing_subscriber::EnvFilter;

const PROVIDER: &str = "A";
const IMAGE_BYTES: usize = 64;
const PROVIDER_MARKER: u8 = 0xA1;

struct Camera;

impl Capability for Camera {
    const TYPE_NAME: &'static str = "Camera";
}

struct CameraA;

#[async_trait]
impl Component for CameraA {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("camera-a")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let capability_instance = CapabilityInstanceId::new();
        let service_name = capability_instance.stream_service_name();
        let descriptor = SliceStream::<Image>::descriptor(service_name, StreamProfile::Sensor);
        let publisher = SliceStream::<Image>::publisher(&descriptor, IMAGE_BYTES)?;
        ctx.provide::<Camera>(capability_instance, descriptor);
        let component = ctx.component().clone();
        let process = ctx.process_instance();
        let activation = ctx.activation_id();
        info!(
            component = %component,
            process_instance = %process,
            activation = %activation,
            capability_instance = %capability_instance,
            provider = PROVIDER,
            "provider activated"
        );

        ctx.spawn_critical("publish", move |token| async move {
            let mut sequence = 1_u64;
            let mut ticker = tokio::time::interval(Duration::from_millis(100));
            ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

            loop {
                tokio::select! {
                    _ = token.cancelled() => {
                        info!(
                            component = %component,
                            process_instance = %process,
                            activation = %activation,
                            capability_instance = %capability_instance,
                            provider = PROVIDER,
                            "provider={} stopped",
                            PROVIDER
                        );
                        return Ok(());
                    }
                    _ = ticker.tick() => {
                        let mut sample = match publisher.loan_slice(IMAGE_BYTES) {
                            Ok(sample) => sample,
                            Err(error) => {
                                error!(
                                    component = %component,
                                    process_instance = %process,
                                    activation = %activation,
                                    capability_instance = %capability_instance,
                                    provider = PROVIDER,
                                    %error,
                                    "provider failed to loan image"
                                );
                                return Err(anyhow!(
                                    "provider={} failed to loan image: {error}",
                                    PROVIDER
                                ));
                            }
                        };
                        {
                            let header = sample.header_mut();
                            header.sequence = sequence;
                            header.timestamp_ns = sequence.saturating_mul(100_000_000);
                            header.width = IMAGE_BYTES as u32;
                            header.height = 1;
                            header.stride = IMAGE_BYTES as u32;
                            header.encoding = PixelFormat::GRAY8;
                        }
                        sample.payload_mut().fill(PROVIDER_MARKER);
                        match sample.send() {
                            Ok(_) => debug!(
                                component = %component,
                                process_instance = %process,
                                activation = %activation,
                                capability_instance = %capability_instance,
                                provider = PROVIDER,
                                sequence,
                                payload_bytes = IMAGE_BYTES,
                                "provider={} published image",
                                PROVIDER
                            ),
                            Err(error) => {
                                error!(
                                    component = %component,
                                    process_instance = %process,
                                    activation = %activation,
                                    capability_instance = %capability_instance,
                                    provider = PROVIDER,
                                    %error,
                                    sequence,
                                    payload_bytes = IMAGE_BYTES,
                                    "provider failed to publish image"
                                );
                                return Err(anyhow!(
                                    "provider={} failed to publish image: {error}",
                                    PROVIDER
                                ));
                            }
                        }
                        sequence = sequence.saturating_add(1);
                    }
                }
            }
        });

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
    run_component::<CameraA, _>((), composer_addr()).await
}

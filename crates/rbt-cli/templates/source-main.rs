use std::time::Duration;

use anyhow::Result;
use async_trait::async_trait;
use rbt::{
    component::{ActivationCtx, Component, run_component},
    stream::SliceStream,
    types::{
        Capability, CapabilityInstanceId, ComponentId, Image, PixelFormat, RequirementSpec,
        StreamProfile,
    },
};

struct Hello;

impl Capability for Hello {
    const TYPE_NAME: &'static str = "Hello";
}

struct HelloSource;

#[async_trait]
impl Component for HelloSource {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("hello-source")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let capability_instance = CapabilityInstanceId::new();
        let descriptor = SliceStream::<Image>::descriptor(
            capability_instance.stream_service_name(),
            StreamProfile::Sensor,
        );
        let publisher = SliceStream::<Image>::publisher(&descriptor, 1)?;
        ctx.provide::<Hello>(capability_instance, descriptor);

        ctx.spawn_critical("publish", move |token| async move {
            let mut sequence = 1_u64;
            let mut ticker = tokio::time::interval(Duration::from_millis(100));
            ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            loop {
                tokio::select! {
                    _ = token.cancelled() => return Ok::<(), anyhow::Error>(()),
                    _ = ticker.tick() => {
                        let mut sample = publisher.loan_slice(1)?;
                        let header = sample.header_mut();
                        header.sequence = sequence;
                        header.timestamp_ns = sequence.saturating_mul(100_000_000);
                        header.width = 1;
                        header.height = 1;
                        header.stride = 1;
                        header.encoding = PixelFormat::GRAY8;
                        sample.payload_mut()[0] = sequence as u8;
                        sample.send()?;
                        sequence = sequence.saturating_add(1);
                    }
                }
            }
        });
        Ok(())
    }
}

#[tokio::main]
async fn main() -> std::result::Result<(), rbt::ControlPlaneError> {
    let address =
        std::env::var("RBT_COMPOSER_ADDR").unwrap_or_else(|_| "127.0.0.1:7447".to_owned());
    run_component::<HelloSource, _>((), address).await
}

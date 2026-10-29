use anyhow::Result;
use async_trait::async_trait;
use rbt::{
    component::{ActivationCtx, Component, run_component},
    stream::SliceStream,
    types::{Capability, ComponentId, Image, RequirementSpec},
};

struct Hello;

impl Capability for Hello {
    const TYPE_NAME: &'static str = "Hello";
}

struct HelloSink;

#[async_trait]
impl Component for HelloSink {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("hello-sink")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        vec![RequirementSpec::typed::<Hello>("hello")]
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let binding = ctx.binding::<Hello>("hello")?;
        let subscriber = SliceStream::<Image>::subscriber(binding.stream())?;
        ctx.spawn_critical("receive", move |token| async move {
            loop {
                tokio::select! {
                    _ = token.cancelled() => return Ok::<(), anyhow::Error>(()),
                    _ = tokio::time::sleep(std::time::Duration::from_millis(10)) => {
                        while let Some(frame) = subscriber.receive()? {
                            println!(
                                "HELLO sequence={} value={}",
                                frame.header().sequence,
                                frame.payload().first().copied().unwrap_or_default()
                            );
                        }
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
    run_component::<HelloSink, _>((), address).await
}

use anyhow::Result;
use async_trait::async_trait;
use rbt::{
    component::{run_component, ActivationCtx, Component},
    types::{Capability, ComponentId, RequirementSpec},
};

/// Replace this role with the capability this component consumes or provides.
#[allow(dead_code)]
struct ExampleCapability;

impl Capability for ExampleCapability {
    const TYPE_NAME: &'static str = "ExampleCapability";
}

struct {{component_type}};

#[async_trait]
impl Component for {{component_type}} {
    type Config = ();
    type Error = anyhow::Error;

    // Preparation runs once per process, before any activation.
    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("{{component_name}}")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        // Declare stable capability roles, not process-local globals. Add this
        // when the component has a real input:
        // vec![RequirementSpec::typed::<ExampleCapability>("input")]
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        // A real component normally resolves its input here:
        // let binding = ctx.binding::<ExampleCapability>("input")?;
        // A provider normally advertises its endpoint here:
        // ctx.provide::<ExampleCapability>(capability_instance, descriptor);
        ctx.spawn_critical("work", move |token| async move {
            loop {
                tokio::select! {
                    _ = token.cancelled() => return Ok::<(), anyhow::Error>(()),
                    _ = tokio::time::sleep(std::time::Duration::from_millis(100)) => {
                        // Activation-owned work belongs here. Cancellation is explicit.
                    }
                }
            }
        });
        Ok(())
    }
}

#[tokio::main]
async fn main() -> std::result::Result<(), rbt::ControlPlaneError> {
    let address = std::env::var("RBT_COMPOSER_ADDR")
        .unwrap_or_else(|_| "127.0.0.1:7447".to_owned());
    run_component::<{{component_type}}, _>((), address).await
}

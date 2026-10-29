use std::time::Duration;

use anyhow::{Result, anyhow, bail};
use async_trait::async_trait;
use rbt::{
    component::{ActivationCtx, Component, run_component},
    stream::{Stream, WaitContext, WaitExit, WaitProgress, WaitToken},
    types::{
        Capability, CapabilityInstanceId, ComponentId, ControlCommand, RequirementSpec,
        StreamProfile,
    },
};
use tracing_subscriber::EnvFilter;

struct MotionCommand;

impl Capability for MotionCommand {
    const TYPE_NAME: &'static str = "MotionCommand";
}

struct Provider;

#[async_trait]
impl Component for Provider {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("control-source")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let capability = CapabilityInstanceId::new();
        let descriptor = Stream::<ControlCommand>::descriptor(
            capability.stream_service_name(),
            StreamProfile::Control,
        );
        let publisher = Stream::<ControlCommand>::publisher(&descriptor)?;
        ctx.provide::<MotionCommand>(capability, descriptor);
        ctx.spawn_critical("publish-control", move |token| async move {
            let mut sequence = 1_u64;
            let mut interval = tokio::time::interval(Duration::from_millis(50));
            interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            loop {
                tokio::select! {
                    _ = token.cancelled() => return Ok::<(), anyhow::Error>(()),
                    _ = interval.tick() => {
                        let mut sample = publisher.loan()?;
                        sample.sequence = sequence;
                        sample.value = sequence as f32 * 0.25;
                        sample.reserved = 0x5242_5400 | sequence as u32 & 0xff;
                        sample.send()?;
                        println!("CONTROL_SOURCE sequence={} value={} reserved={}", sequence, sequence as f32 * 0.25, 0x5242_5400 | sequence as u32 & 0xff);
                        sequence = sequence.saturating_add(1);
                    }
                }
            }
        });
        Ok(())
    }
}

struct Consumer;

#[async_trait]
impl Component for Consumer {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("control-sink")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        vec![RequirementSpec::typed::<MotionCommand>("commands")]
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let binding = ctx.binding::<MotionCommand>("commands")?;
        let subscriber = Stream::<ControlCommand>::subscriber(binding.stream())?;
        let mut wait = WaitContext::new(ctx.process_instance(), ctx.activation_id())?;
        let wake = wait.wake_handle();
        ctx.spawn_critical_blocking(
            "receive-control",
            move |token| {
                let source = subscriber.event_source(WaitToken::new(1));
                match wait.run(
                    std::slice::from_ref(&source),
                    |event, budget| -> Result<WaitProgress> {
                        if event.is_cancellation() || token.is_cancelled() {
                            return Ok(WaitProgress::continue_waiting());
                        }
                        let mut drained = 0;
                        for _ in 0..budget {
                            let Some(sample) = subscriber.receive()? else {
                                return Ok(if drained == 0 {
                                    WaitProgress::empty()
                                } else {
                                    WaitProgress::samples(drained)
                                });
                            };
                            println!(
                                "CONTROL_SINK sequence={} value={} reserved={}",
                                sample.sequence, sample.value, sample.reserved
                            );
                            drained += 1;
                        }
                        Ok(WaitProgress::samples(drained))
                    },
                ) {
                    Ok(WaitExit::Cancelled | WaitExit::Stopped) => Ok(()),
                    Err(error) => Err(anyhow!("control receive failed: {error}")),
                }
            },
            move || {
                let _ = wake.wake();
            },
        );
        Ok(())
    }
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
    match std::env::args().nth(1).as_deref() {
        Some("provider") => run_component::<Provider, _>((), composer_addr())
            .await
            .map_err(Into::into),
        Some("consumer") => run_component::<Consumer, _>((), composer_addr())
            .await
            .map_err(Into::into),
        _ => bail!("usage: control-stream <provider|consumer>"),
    }
}

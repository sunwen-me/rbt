//! Finite local proxy acceptance against the existing Composer protocol.
//!
//! The network observer uses fork-native metadata only. The Composer and
//! dependent run the unchanged production runtime. This is a control-plane
//! prototype, not a 0.9/0.10 shared-memory or SDK interoperability claim.

use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    thread,
    time::Duration,
};

use anyhow::{Context, Result, bail, ensure};
use async_trait::async_trait;
use cross_host_upstream_probe::ImageHeader;
use iceoryx2::prelude::*;
use rbt_compose::{
    ActivationCtx, ActivationSnapshot, CapabilitySnapshot, ClientMessage, Component,
    ComponentSnapshot, InspectionView, RequirementSnapshot, ServerMessage, run_component_until,
};
use rbt_core::{
    ActivationId, Capability, CapabilityDescriptor, CapabilityInstanceId, ComponentId,
    ComponentState, ProcessInstanceId, RequirementSpec, StreamDescriptor,
};
use rbt_types::{Image, StreamProfile};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader},
    net::TcpStream,
    sync::watch,
};

struct Camera;
impl Capability for Camera {
    const TYPE_NAME: &'static str = "Camera";
}

#[derive(Default)]
struct Counts {
    prepared: AtomicUsize,
    activated: AtomicUsize,
}
struct Detector {
    counts: Arc<Counts>,
}

#[async_trait]
impl Component for Detector {
    type Config = Arc<Counts>;
    type Error = anyhow::Error;
    async fn prepare(counts: &Self::Config) -> Result<Self> {
        counts.prepared.fetch_add(1, Ordering::SeqCst);
        Ok(Self {
            counts: Arc::clone(counts),
        })
    }
    fn id(&self) -> ComponentId {
        ComponentId::new("fork-detector")
    }
    fn requirements(&self) -> Vec<RequirementSpec> {
        vec![RequirementSpec::new("camera", "Camera")]
    }
    async fn activate(&mut self, context: &mut ActivationCtx<'_>) -> Result<()> {
        let _binding = context.binding::<Camera>("camera")?;
        self.counts.activated.fetch_add(1, Ordering::SeqCst);
        Ok(())
    }
}

async fn send(writer: &mut tokio::net::tcp::OwnedWriteHalf, message: &ClientMessage) -> Result<()> {
    let mut line = serde_json::to_vec(message)?;
    line.push(b'\n');
    writer.write_all(&line).await?;
    Ok(())
}

async fn inspect(address: &str) -> Result<InspectionView> {
    let stream = TcpStream::connect(address).await?;
    let (reader, mut writer) = stream.into_split();
    send(&mut writer, &ClientMessage::inspect()).await?;
    let mut reader = BufReader::new(reader);
    let mut line = String::new();
    reader.read_line(&mut line).await?;
    match serde_json::from_str(&line)? {
        ServerMessage::InspectResponse { view } => Ok(view),
        other => bail!("unexpected inspection reply: {other:?}"),
    }
}

async fn proxy(address: String, service: String, mut health: watch::Receiver<bool>) -> Result<()> {
    let stream = TcpStream::connect(&address).await?;
    let (reader, mut writer) = stream.into_split();
    let mut reader = BufReader::new(reader);
    let process = ProcessInstanceId::new();
    let component = ComponentId::new("fork-import-proxy");
    send(
        &mut writer,
        &ClientMessage::hello(component.clone(), process),
    )
    .await?;
    let mut activation = None;
    let mut next_activation = ActivationId::first();
    let mut capability = None;
    let mut line = String::new();

    loop {
        let is_healthy = *health.borrow_and_update();
        if activation.is_some() {
            if is_healthy && capability.is_none() {
                let instance = CapabilityInstanceId::new();
                ensure!(
                    !service.contains(&instance.to_string()),
                    "local proxy reused remote identity"
                );
                capability = Some(instance);
                println!(
                    "PROXY health=present remote_service={service} local_capability={instance} process={process}"
                );
            } else if !is_healthy && capability.take().is_some() {
                println!(
                    "PROXY health=absent remote_service={service} capability=revoked process={process}"
                );
            }
        }
        let capabilities = match (activation, capability) {
            (Some(active), Some(instance)) => vec![CapabilitySnapshot::new(
                Camera::TYPE_NAME,
                instance,
                CapabilityDescriptor::new(
                    instance,
                    component.clone(),
                    process,
                    active,
                    StreamDescriptor::new(
                        service.clone(),
                        Image::descriptor(),
                        StreamProfile::Sensor,
                    ),
                ),
            )],
            _ => vec![],
        };
        let snapshot = ComponentSnapshot::new(
            component.clone(),
            process,
            if activation.is_some() {
                ComponentState::Active
            } else {
                ComponentState::Blocked("awaiting activation".into())
            },
            activation.map(|active| ActivationSnapshot::new(active, [])),
            std::iter::empty::<RequirementSnapshot>(),
            capabilities,
        );
        send(&mut writer, &ClientMessage::snapshot(snapshot)).await?;

        line.clear();
        tokio::select! {
            changed = health.changed() => { changed.context("remote health observer stopped")?; }
            read = reader.read_line(&mut line) => {
                ensure!(read? > 0, "Composer closed the prototype proxy connection");
                match serde_json::from_str::<ServerMessage>(&line)? {
                    ServerMessage::Activate { .. } => {
                        ensure!(activation.is_none(), "proxy received duplicate activation");
                        activation = Some(next_activation);
                        next_activation = next_activation.next();
                    }
                    ServerMessage::Deactivate { .. } => { activation = None; capability = None; }
                    message => bail!("unexpected proxy command {message:?}"),
                }
            }
        }
    }
}

fn observe(service: String, health: watch::Sender<bool>, stop: Arc<AtomicBool>) -> Result<()> {
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let name = ServiceName::new(&service)?;
    let mut factory = None;
    while !stop.load(Ordering::Acquire) {
        if factory.is_none() {
            factory = node
                .service_builder(&name)
                .publish_subscribe::<[u8]>()
                .user_header::<ImageHeader>()
                .subscriber_max_buffer_size(1)
                .subscriber_max_borrowed_samples(1)
                .enable_safe_overflow(true)
                .history_size(0)
                .open()
                .ok();
        }
        // Holding this factory deliberately verifies that a consumer-held
        // mirror does not become a new export after withdrawal/recovery.
        let present = factory
            .as_ref()
            .is_some_and(|factory| factory.dynamic_config().number_of_publishers() > 0);
        health.send_if_modified(|value| {
            if *value == present {
                false
            } else {
                *value = present;
                true
            }
        });
        // This is low-rate native health metadata observation. It never reads
        // payloads, drives tunnel progress, or treats payload silence as loss.
        thread::sleep(Duration::from_millis(50));
    }
    Ok(())
}

pub async fn run(service: String, timeout_ms: u64, expect_recovery: bool) -> Result<()> {
    let reservation = std::net::TcpListener::bind("127.0.0.1:0")?;
    let address = reservation.local_addr()?.to_string();
    drop(reservation);
    let composer_address = address.clone();
    let composer = tokio::spawn(async move { rbt_compose::serve(composer_address).await });
    for _ in 0..100 {
        if TcpStream::connect(&address).await.is_ok() {
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    let counts = Arc::new(Counts::default());
    let detector_counts = Arc::clone(&counts);
    let detector_address = address.clone();
    let detector = tokio::spawn(async move {
        run_component_until::<Detector, _, _>(
            detector_counts,
            detector_address,
            std::future::pending(),
        )
        .await
    });
    let (health_tx, health_rx) = watch::channel(false);
    let stop = Arc::new(AtomicBool::new(false));
    let observer_stop = Arc::clone(&stop);
    let observer_service = service.clone();
    let observer = thread::spawn(move || observe(observer_service, health_tx, observer_stop));
    let proxy_task = tokio::spawn(proxy(address.clone(), service, health_rx));
    let deadline = tokio::time::Instant::now() + Duration::from_millis(timeout_ms);
    let mut stage = 0;
    let mut detector_process = None;
    let mut first_proxy = None;

    let result = async {
        loop {
            ensure!(tokio::time::Instant::now() < deadline, "lifecycle timeout at stage={stage}");
            ensure!(!proxy_task.is_finished(), "prototype proxy stopped unexpectedly");
            let view = tokio::time::timeout(Duration::from_secs(1), inspect(&address)).await??;
            if let Some(component) = view.components.iter().find(|value| value.component_id.as_str() == "fork-detector") {
                if let Some(previous) = detector_process { ensure!(previous == component.snapshot.process_instance, "dependent process restarted"); }
                detector_process = Some(component.snapshot.process_instance);
                let binding = component.snapshot.activation.as_ref().and_then(|activation| activation.bindings.first());
                match (stage, &component.snapshot.state) {
                    (0, ComponentState::Active) => {
                        let binding = binding.context("active detector has no Camera binding")?;
                        first_proxy = Some(binding.descriptor.capability_instance());
                        println!("LIFECYCLE phase=active process={} proxy={} prepare_count={}", component.snapshot.process_instance,
                            binding.descriptor.capability_instance(), counts.prepared.load(Ordering::SeqCst));
                        stage = 1;
                    }
                    (1, ComponentState::Blocked(_)) => {
                        ensure!(component.snapshot.activation.is_none(), "blocked detector retains activation");
                        ensure!(view.capability_topology.iter().all(|capability| capability.capability_type != "Camera"), "stale Camera capability");
                        println!("LIFECYCLE phase=blocked process={} prepare_count={}", component.snapshot.process_instance, counts.prepared.load(Ordering::SeqCst));
                        stage = 2;
                        if !expect_recovery { break; }
                    }
                    (2, ComponentState::Active) => {
                        let binding = binding.context("reactivated detector has no binding")?;
                        ensure!(Some(binding.descriptor.capability_instance()) != first_proxy, "replacement reused proxy identity");
                        println!("LIFECYCLE phase=reactivated process={} proxy={} prepare_count={}", component.snapshot.process_instance,
                            binding.descriptor.capability_instance(), counts.prepared.load(Ordering::SeqCst));
                        stage = 3;
                    }
                    (3, ComponentState::Blocked(_)) => {
                        ensure!(component.snapshot.activation.is_none(), "second loss retained activation");
                        ensure!(view.capability_topology.iter().all(|capability| capability.capability_type != "Camera"), "stale Camera capability after second loss");
                        println!("LIFECYCLE phase=blocked-again process={} prepare_count={}", component.snapshot.process_instance, counts.prepared.load(Ordering::SeqCst));
                        stage = 4;
                        break;
                    }
                    _ => {}
                }
            }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        ensure!(counts.prepared.load(Ordering::SeqCst) == 1, "dependent prepare ran again");
        ensure!(counts.activated.load(Ordering::SeqCst) == if expect_recovery { 2 } else { 1 }, "unexpected dependent activation count");
        println!("SUMMARY kind=lifecycle result=PASS stages={stage} prepare_count=1 process_preserved=true recovery={expect_recovery} real_composer=true real_dependent=true");
        Ok(())
    }.await;
    stop.store(true, Ordering::Release);
    let observer_result = observer
        .join()
        .map_err(|_| anyhow::anyhow!("observer panicked"))?;
    proxy_task.abort();
    detector.abort();
    composer.abort();
    observer_result?;
    result
}

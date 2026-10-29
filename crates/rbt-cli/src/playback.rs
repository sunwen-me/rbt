use std::{
    collections::BTreeMap,
    path::PathBuf,
    sync::{Arc, Condvar, Mutex},
    time::{Duration, Instant},
};

use async_trait::async_trait;
use rbt_compose::{ActivationCtx, Component, run_component_until};
use rbt_core::{CapabilityInstanceId, ComponentId, RequirementSpec};
use rbt_iox2::{
    ControlCommand, Image, PointCloud, SlicePublisher, SliceStream, Stream, StreamProfile,
};
use rbt_mcap::{
    CatalogChannel, KnownMessageType, McapMessage, RecordingCatalog, decode_control_into,
    decode_image_header_into, decode_image_payload_into, decode_pointcloud_header_into,
    decode_pointcloud_payload_into, payload_element_count, scan_recording, visit_messages_with,
};
use rbt_types::{ImageHeader, PointCloudHeader};
use serde::Serialize;
use tokio::sync::oneshot;
use tokio_util::sync::CancellationToken;

use crate::CliError;

const SUBSCRIBER_STARTUP_GRACE: Duration = Duration::from_millis(250);

type CompletionSender = Arc<Mutex<Option<oneshot::Sender<Result<PlaySummary, String>>>>>;

/// Final finite-playback counters.
#[derive(Clone, Debug, Serialize)]
pub struct PlaySummary {
    pub input: PathBuf,
    pub rate: f64,
    pub messages_published: u64,
    pub recipients_reported: u64,
    pub recorded_duration_ns: u64,
    pub elapsed_ns: u64,
}

#[derive(Debug, thiserror::Error)]
enum PlayError {
    #[error(transparent)]
    Mcap(#[from] rbt_mcap::Error),
    #[error(transparent)]
    Stream(#[from] rbt_iox2::StreamError),
    #[error("playback was cancelled")]
    Cancelled,
    #[error("invalid playback catalog: {0}")]
    Catalog(String),
    #[error("playback schedule exceeds the monotonic clock range")]
    TimingOverflow,
}

#[derive(Clone)]
struct ChannelGroup {
    topic: String,
    channel_ids: Vec<u16>,
    kind: KnownMessageType,
    profile: StreamProfile,
    capability_type: String,
    capability_name: String,
    capability_qualifier: String,
    maximum_elements: usize,
}

struct PlayerConfig {
    input: PathBuf,
    rate: f64,
    skip_unsupported: bool,
    catalog: RecordingCatalog,
    groups: Vec<ChannelGroup>,
    completion: CompletionSender,
}

struct PlayerComponent {
    input: PathBuf,
    rate: f64,
    skip_unsupported: bool,
    catalog: RecordingCatalog,
    groups: Vec<ChannelGroup>,
    completion: CompletionSender,
}

struct PlaybackRun {
    input: PathBuf,
    rate: f64,
    skip_unsupported: bool,
    first_log_time: u64,
    recorded_duration_ns: u64,
    channel_to_group: BTreeMap<u16, usize>,
    publishers: Vec<GroupPublisher>,
    token: CancellationToken,
    wake_state: Arc<(Mutex<bool>, Condvar)>,
}

enum GroupPublisher {
    Image(SlicePublisher<Image>),
    PointCloud(SlicePublisher<PointCloud>),
    Control(rbt_iox2::Publisher<ControlCommand>),
}

impl GroupPublisher {
    fn publish(&self, kind: KnownMessageType, data: &[u8]) -> Result<usize, PlayError> {
        match (self, kind) {
            (Self::Image(publisher), KnownMessageType::Image) => {
                let count = payload_element_count(kind, data)?;
                let mut loan = publisher.loan_slice(count)?;
                let mut header = ImageHeader::default();
                decode_image_header_into(data, &mut header)?;
                *loan.header_mut() = header;
                decode_image_payload_into(data, loan.payload_mut())?;
                // Exactly one send attempt. QueueFull, UnableToDeliver, and
                // ReadinessNotify may be non-transactional and are never retried.
                attempt_send(|| loan.send())
            }
            (Self::PointCloud(publisher), KnownMessageType::PointCloud) => {
                let count = payload_element_count(kind, data)?;
                let mut loan = publisher.loan_slice(count)?;
                let mut header = PointCloudHeader::default();
                decode_pointcloud_header_into(data, &mut header)?;
                *loan.header_mut() = header;
                decode_pointcloud_payload_into(data, loan.payload_mut())?;
                attempt_send(|| loan.send())
            }
            (Self::Control(publisher), KnownMessageType::ControlCommand) => {
                let mut loan = publisher.loan()?;
                decode_control_into(data, &mut loan)?;
                attempt_send(|| loan.send())
            }
            _ => Err(PlayError::Catalog(
                "channel was mapped to a publisher with a different message type".to_owned(),
            )),
        }
    }
}

fn attempt_send(
    send: impl FnOnce() -> Result<usize, rbt_iox2::StreamError>,
) -> Result<usize, PlayError> {
    // Every transport outcome is terminal for this message. In particular,
    // UnableToDeliver and ReadinessNotify can follow partial payload delivery,
    // so replay must never invoke `send` a second time.
    send().map_err(PlayError::Stream)
}

#[async_trait]
impl Component for PlayerComponent {
    type Config = PlayerConfig;
    type Error = PlayError;

    async fn prepare(config: &Self::Config) -> Result<Self, Self::Error> {
        Ok(Self {
            input: config.input.clone(),
            rate: config.rate,
            skip_unsupported: config.skip_unsupported,
            catalog: config.catalog.clone(),
            groups: config.groups.clone(),
            completion: Arc::clone(&config.completion),
        })
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("rbt-player")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<(), Self::Error> {
        let mut publishers = Vec::with_capacity(self.groups.len());
        let mut channel_to_group = BTreeMap::new();
        for (group_index, group) in self.groups.iter().enumerate() {
            let capability_instance = CapabilityInstanceId::new();
            let service_name = capability_instance.stream_service_name();
            let descriptor = match group.kind {
                KnownMessageType::Image => {
                    let descriptor = SliceStream::<Image>::descriptor(service_name, group.profile);
                    let publisher = SliceStream::<Image>::publisher(
                        &descriptor,
                        group.maximum_elements.max(1),
                    )?;
                    publishers.push(GroupPublisher::Image(publisher));
                    descriptor
                }
                KnownMessageType::PointCloud => {
                    let descriptor =
                        SliceStream::<PointCloud>::descriptor(service_name, group.profile);
                    let publisher = SliceStream::<PointCloud>::publisher(
                        &descriptor,
                        group.maximum_elements.max(1),
                    )?;
                    publishers.push(GroupPublisher::PointCloud(publisher));
                    descriptor
                }
                KnownMessageType::ControlCommand => {
                    let descriptor =
                        Stream::<ControlCommand>::descriptor(service_name, group.profile);
                    let publisher = Stream::<ControlCommand>::publisher(&descriptor)?;
                    publishers.push(GroupPublisher::Control(publisher));
                    descriptor
                }
            };
            ctx.provide_untyped(
                group.capability_type.clone(),
                capability_instance,
                descriptor,
            );
            for channel_id in &group.channel_ids {
                channel_to_group.insert(*channel_id, group_index);
            }
        }

        let input = self.input.clone();
        let rate = self.rate;
        let skip_unsupported = self.skip_unsupported;
        let first_log_time = self.catalog.first_log_time.unwrap_or(0);
        let recorded_duration_ns = self
            .catalog
            .last_log_time
            .zip(self.catalog.first_log_time)
            .map_or(0, |(last, first)| last.saturating_sub(first));
        let completion = Arc::clone(&self.completion);
        let wake_state = Arc::new((Mutex::new(false), Condvar::new()));
        let task_wake = Arc::clone(&wake_state);
        let cancel_wake = Arc::clone(&wake_state);
        ctx.spawn_critical_blocking(
            "mcap-playback",
            move |token| {
                let result = play_file(PlaybackRun {
                    input,
                    rate,
                    skip_unsupported,
                    first_log_time,
                    recorded_duration_ns,
                    channel_to_group,
                    publishers,
                    token: token.clone(),
                    wake_state: task_wake,
                });
                let reported = result
                    .as_ref()
                    .map(Clone::clone)
                    .map_err(ToString::to_string);
                if let Some(sender) = completion.lock().ok().and_then(|mut value| value.take()) {
                    let _ = sender.send(reported);
                }
                match result {
                    Ok(_) => {
                        // Completion shuts down the finite component runner.
                        // Wait for its cancellation so natural task exit is not
                        // misclassified as an unexpected critical failure.
                        wait_for_cancellation(&token, &cancel_wake);
                        Ok(())
                    }
                    Err(error) => Err(error),
                }
            },
            move || {
                if let Ok(mut awakened) = wake_state.0.lock() {
                    *awakened = true;
                    wake_state.1.notify_all();
                }
            },
        );
        Ok(())
    }
}

/// Preflights a file, launches a fresh native provider component, and waits for completion.
pub async fn run_play(
    input: PathBuf,
    rate: f64,
    skip_unsupported: bool,
    composer_addr: String,
) -> Result<PlaySummary, CliError> {
    if !rate.is_finite() || rate < 0.0 {
        return Err(CliError::Usage(
            "playback rate must be finite and non-negative".to_owned(),
        ));
    }
    // Full schema/ABI/payload preflight occurs before any SHM publisher exists.
    let catalog = scan_recording(&input, skip_unsupported)?;
    let groups = group_channels(&catalog)?;
    if groups.is_empty() {
        return Err(CliError::Playback(
            "recording contains no supported rbt channels".to_owned(),
        ));
    }

    let (completion_tx, completion_rx) = oneshot::channel();
    let completion = Arc::new(Mutex::new(Some(completion_tx)));
    let outcome = Arc::new(Mutex::new(None));
    let shutdown_outcome = Arc::clone(&outcome);
    let shutdown = async move {
        let result = tokio::select! {
            result = completion_rx => result
                .unwrap_or_else(|_| Err("playback completion channel closed".to_owned())),
            signal = shutdown_signal() => match signal {
                Ok(()) => Err("playback interrupted".to_owned()),
                Err(error) => Err(format!("failed to install shutdown signal handler: {error}")),
            },
        };
        if let Ok(mut destination) = shutdown_outcome.lock() {
            *destination = Some(result);
        }
    };
    run_component_until::<PlayerComponent, _, _>(
        PlayerConfig {
            input,
            rate,
            skip_unsupported,
            catalog,
            groups,
            completion,
        },
        composer_addr,
        shutdown,
    )
    .await?;
    outcome
        .lock()
        .map_err(|_| CliError::Playback("playback result lock was poisoned".to_owned()))?
        .take()
        .ok_or_else(|| CliError::Playback("playback exited without a result".to_owned()))?
        .map_err(CliError::Playback)
}

fn group_channels(catalog: &RecordingCatalog) -> Result<Vec<ChannelGroup>, CliError> {
    let mut groups = Vec::<ChannelGroup>::new();
    for channel in catalog
        .channels
        .iter()
        .filter(|channel| channel.known_type.is_some())
    {
        let candidate = channel_group(channel)?;
        if let Some(existing) = groups
            .iter_mut()
            .find(|group| group.topic == candidate.topic)
        {
            if existing.kind != candidate.kind
                || existing.profile != candidate.profile
                || existing.capability_type != candidate.capability_type
                || existing.capability_name != candidate.capability_name
                || existing.capability_qualifier != candidate.capability_qualifier
            {
                return Err(CliError::Playback(format!(
                    "logical topic {:?} has incompatible channel segments",
                    candidate.topic
                )));
            }
            existing.channel_ids.extend(candidate.channel_ids);
            existing.maximum_elements = existing.maximum_elements.max(candidate.maximum_elements);
        } else {
            groups.push(candidate);
        }
    }
    Ok(groups)
}

fn channel_group(channel: &CatalogChannel) -> Result<ChannelGroup, CliError> {
    Ok(ChannelGroup {
        topic: channel.topic.clone(),
        channel_ids: vec![channel.id],
        kind: channel
            .known_type
            .ok_or_else(|| CliError::Playback("unsupported channel entered grouping".to_owned()))?,
        profile: channel
            .profile
            .ok_or_else(|| CliError::Playback("channel is missing stream profile".to_owned()))?,
        capability_type: channel.capability_type.clone().ok_or_else(|| {
            CliError::Playback("channel is missing logical capability type".to_owned())
        })?,
        capability_name: channel.capability_name.clone().unwrap_or_default(),
        capability_qualifier: channel.capability_qualifier.clone().unwrap_or_default(),
        maximum_elements: channel.maximum_elements,
    })
}

fn play_file(run: PlaybackRun) -> Result<PlaySummary, PlayError> {
    let PlaybackRun {
        input,
        rate,
        skip_unsupported,
        first_log_time,
        recorded_duration_ns,
        channel_to_group,
        publishers,
        token,
        wake_state,
    } = run;
    wait_for_delay(SUBSCRIBER_STARTUP_GRACE, &token, &wake_state)?;
    let started = Instant::now();
    let mut messages_published = 0_u64;
    let mut recipients_reported = 0_u64;
    let mut last_progress = started;
    visit_messages_with::<PlayError, _>(&input, skip_unsupported, |channel, message| {
        if token.is_cancelled() {
            return Err(PlayError::Cancelled);
        }
        wait_for_message(started, first_log_time, message, rate, &token, &wake_state)?;
        let group_index = *channel_to_group.get(&channel.id).ok_or_else(|| {
            PlayError::Catalog(format!("channel {} has no publisher group", channel.id))
        })?;
        let recipients = publishers[group_index].publish(
            channel
                .known_type
                .ok_or_else(|| PlayError::Catalog("unsupported channel was visited".to_owned()))?,
            message.data,
        )?;
        messages_published = messages_published.saturating_add(1);
        recipients_reported = recipients_reported.saturating_add(recipients as u64);
        if last_progress.elapsed() >= Duration::from_secs(1) {
            eprintln!(
                "play progress elapsed={:.3}s recorded={:.3}s rate={} messages={} channel={}",
                started.elapsed().as_secs_f64(),
                message.log_time.saturating_sub(first_log_time) as f64 / 1_000_000_000.0,
                rate,
                messages_published,
                channel.topic,
            );
            last_progress = Instant::now();
        }
        Ok(())
    })?;
    Ok(PlaySummary {
        input,
        rate,
        messages_published,
        recipients_reported,
        recorded_duration_ns,
        elapsed_ns: u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX),
    })
}

fn wait_for_delay(
    delay: Duration,
    token: &CancellationToken,
    wake_state: &Arc<(Mutex<bool>, Condvar)>,
) -> Result<(), PlayError> {
    let target = Instant::now()
        .checked_add(delay)
        .ok_or(PlayError::TimingOverflow)?;
    let (lock, condvar) = &**wake_state;
    let mut awakened = lock
        .lock()
        .map_err(|_| PlayError::Catalog("playback timer lock was poisoned".to_owned()))?;
    while !*awakened && !token.is_cancelled() {
        let now = Instant::now();
        if now >= target {
            return Ok(());
        }
        let (next, _) = condvar
            .wait_timeout(awakened, target.duration_since(now))
            .map_err(|_| PlayError::Catalog("playback timer lock was poisoned".to_owned()))?;
        awakened = next;
    }
    Err(PlayError::Cancelled)
}

fn wait_for_message(
    started: Instant,
    first_log_time: u64,
    message: McapMessage<'_>,
    rate: f64,
    token: &CancellationToken,
    wake_state: &Arc<(Mutex<bool>, Condvar)>,
) -> Result<(), PlayError> {
    if rate == 0.0 {
        return Ok(());
    }
    let recorded_delta = message.log_time.saturating_sub(first_log_time);
    let scaled_ns = (recorded_delta as f64 / rate).round();
    if !scaled_ns.is_finite() || scaled_ns > u64::MAX as f64 {
        return Err(PlayError::TimingOverflow);
    }
    let target = started
        .checked_add(Duration::from_nanos(scaled_ns as u64))
        .ok_or(PlayError::TimingOverflow)?;
    let (lock, condvar) = &**wake_state;
    let mut awakened = lock
        .lock()
        .map_err(|_| PlayError::Catalog("playback timer lock was poisoned".to_owned()))?;
    while !*awakened && !token.is_cancelled() {
        let now = Instant::now();
        if now >= target {
            return Ok(());
        }
        let (next, _) = condvar
            .wait_timeout(awakened, target.duration_since(now))
            .map_err(|_| PlayError::Catalog("playback timer lock was poisoned".to_owned()))?;
        awakened = next;
    }
    Err(PlayError::Cancelled)
}

fn wait_for_cancellation(token: &CancellationToken, wake_state: &Arc<(Mutex<bool>, Condvar)>) {
    let (lock, condvar) = &**wake_state;
    if let Ok(mut awakened) = lock.lock() {
        while !*awakened && !token.is_cancelled() {
            match condvar.wait(awakened) {
                Ok(next) => awakened = next,
                Err(_) => return,
            }
        }
    }
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

#[cfg(test)]
mod tests {
    use super::*;
    use rbt_mcap::encode_control;

    #[test]
    fn control_partial_delivery_is_not_retried_or_duplicated() {
        let capability = CapabilityInstanceId::new();
        let descriptor = Stream::<ControlCommand>::descriptor(
            capability.stream_service_name(),
            StreamProfile::Control,
        );
        let publisher = Stream::<ControlCommand>::publisher(&descriptor).unwrap();
        let fast_a = Stream::<ControlCommand>::subscriber(&descriptor).unwrap();
        let fast_b = Stream::<ControlCommand>::subscriber(&descriptor).unwrap();
        let stalled = Stream::<ControlCommand>::subscriber(&descriptor).unwrap();
        let publisher = GroupPublisher::Control(publisher);

        for sequence in 0..4 {
            let command = encode_control(&ControlCommand {
                sequence,
                value: sequence as f32,
                reserved: 0,
            });
            assert_eq!(
                publisher
                    .publish(KnownMessageType::ControlCommand, &command)
                    .unwrap(),
                3
            );
            assert_eq!(fast_a.receive().unwrap().unwrap().sequence, sequence);
            assert_eq!(fast_b.receive().unwrap().unwrap().sequence, sequence);
        }
        let fifth = encode_control(&ControlCommand {
            sequence: 4,
            value: 4.0,
            reserved: 0,
        });
        assert!(matches!(
            publisher.publish(KnownMessageType::ControlCommand, &fifth),
            Err(PlayError::Stream(rbt_iox2::StreamError::QueueFull))
        ));
        // The Control send is non-transactional: fast peers received sequence
        // 4 even though the stalled peer made the aggregate call fail.
        assert_eq!(fast_a.receive().unwrap().unwrap().sequence, 4);
        assert_eq!(fast_b.receive().unwrap().unwrap().sequence, 4);
        assert!(fast_a.receive().unwrap().is_none());
        assert!(fast_b.receive().unwrap().is_none());
        for sequence in 0..4 {
            assert_eq!(stalled.receive().unwrap().unwrap().sequence, sequence);
        }
        assert!(stalled.receive().unwrap().is_none());
        let GroupPublisher::Control(publisher) = publisher else {
            unreachable!()
        };
        let pressure = publisher.pressure().publisher.unwrap();
        assert_eq!(pressure.send_attempts, 5);
        assert_eq!(pressure.send_successes, 4);
        assert_eq!(pressure.send_failures, 1);
    }

    #[test]
    fn queue_full_unable_to_deliver_and_readiness_notify_each_get_one_attempt() {
        fn one_attempt(error: rbt_iox2::StreamError) -> PlayError {
            let attempts = std::cell::Cell::new(0_u32);
            let result = attempt_send(|| {
                attempts.set(attempts.get() + 1);
                Err(error)
            });
            assert_eq!(attempts.get(), 1);
            result.unwrap_err()
        }

        assert!(matches!(
            one_attempt(rbt_iox2::StreamError::QueueFull),
            PlayError::Stream(rbt_iox2::StreamError::QueueFull)
        ));
        assert!(matches!(
            one_attempt(rbt_iox2::StreamError::UnableToDeliver),
            PlayError::Stream(rbt_iox2::StreamError::UnableToDeliver)
        ));
        assert!(matches!(
            one_attempt(rbt_iox2::StreamError::ReadinessNotify(
                iceoryx2::port::notifier::NotifierNotifyError::MissedDeadline,
            )),
            PlayError::Stream(rbt_iox2::StreamError::ReadinessNotify(
                iceoryx2::port::notifier::NotifierNotifyError::MissedDeadline,
            ))
        ));
    }
}

use std::time::Duration;

use rbt_compose::{ActivationCtx, ScopedTaskHandle};
use rbt_core::{Capability, CapabilityInstanceId};
use rbt_iox2::{SliceStream, Stream, StreamProfile, WaitContext, WaitProgress, WaitToken};
use rbt_tf::{StaticSet, TransformBuffer};
use rbt_types::{
    DynamicTransform, RpcDescriptor, RpcResourceConfig, StaticSnapshot, StaticSnapshotRequest,
    StaticUpdate,
};
use tokio_util::sync::CancellationToken;

use crate::{
    DynamicTfPublisher, DynamicTfSubscriber, StaticTfClient, StaticTfProvider,
    StaticTfProviderControl, TransportError, TransportResult,
};

/// Endpoints staged by an activation for one static TF provider.
#[derive(Clone, Debug)]
pub struct ProvidedStaticTf {
    /// Physical identity of the static update stream and graph authority.
    pub update_authority: CapabilityInstanceId,
    /// Physical identity of the paired snapshot RPC endpoint.
    pub snapshot_capability: CapabilityInstanceId,
    /// Handle used by the component to atomically replace its static set.
    pub control: StaticTfProviderControl,
    /// Activation-scoped server loop.
    pub task: ScopedTaskHandle,
}

/// A dynamic publisher and its freshly advertised physical capability identity.
#[derive(Clone, Debug)]
pub struct ProvidedDynamicTf {
    /// Physical stream identity written into native records.
    pub capability: CapabilityInstanceId,
    /// Native publisher for complete dynamic transforms.
    pub publisher: DynamicTfPublisher,
}

/// A static consumer binding owned by one component activation.
#[derive(Clone, Debug)]
pub struct StaticTfBinding {
    /// Logical buffer authority token for this binding.
    pub authority: rbt_tf::Authority,
    /// Background update and periodic snapshot task.
    pub task: ScopedTaskHandle,
}

/// A dynamic consumer binding owned by one component activation.
#[derive(Clone, Debug)]
pub struct DynamicTfBinding {
    /// Logical buffer authority token for this binding.
    pub authority: rbt_tf::Authority,
    /// Background stream ingestor task.
    pub task: ScopedTaskHandle,
}

/// TF-specific activation helpers over the existing Composer capability roles.
pub trait TransformActivationExt {
    /// Opens and advertises the update stream and snapshot RPC for a static provider.
    fn provide_static_tf<UpdateRole, SnapshotRole>(
        &mut self,
        initial: StaticSet,
        maximum_transforms: usize,
        profile: StreamProfile,
    ) -> TransportResult<ProvidedStaticTf>
    where
        UpdateRole: Capability,
        SnapshotRole: Capability;

    /// Opens, advertises, and activation-scopes one dynamic publisher.
    fn provide_dynamic_tf<DynamicRole>(
        &mut self,
        profile: StreamProfile,
    ) -> TransportResult<ProvidedDynamicTf>
    where
        DynamicRole: Capability;

    /// Resolves and validates the update/RPC pair, then owns its ingestor and cleanup in the scope.
    fn require_static_tf<UpdateRole, SnapshotRole>(
        &mut self,
        update_requirement: impl AsRef<str>,
        snapshot_requirement: impl AsRef<str>,
        slot: &str,
        maximum_transforms: usize,
        buffer: TransformBuffer,
    ) -> TransportResult<StaticTfBinding>
    where
        UpdateRole: Capability,
        SnapshotRole: Capability;

    /// Resolves one dynamic stream and owns its ingestor and cleanup in the scope.
    fn require_dynamic_tf<DynamicRole>(
        &mut self,
        requirement: impl AsRef<str>,
        slot: &str,
        buffer: TransformBuffer,
    ) -> TransportResult<DynamicTfBinding>
    where
        DynamicRole: Capability;
}

impl TransformActivationExt for ActivationCtx<'_> {
    fn provide_static_tf<UpdateRole, SnapshotRole>(
        &mut self,
        initial: StaticSet,
        maximum_transforms: usize,
        profile: StreamProfile,
    ) -> TransportResult<ProvidedStaticTf>
    where
        UpdateRole: Capability,
        SnapshotRole: Capability,
    {
        if UpdateRole::TYPE_NAME == SnapshotRole::TYPE_NAME {
            return Err(TransportError::PairingInvalid(
                "static update and snapshot must use distinct capability roles",
            ));
        }
        let update_authority = CapabilityInstanceId::new();
        let snapshot_capability = CapabilityInstanceId::new();
        let update_descriptor = SliceStream::<StaticUpdate>::descriptor(
            update_authority.stream_service_name(),
            profile,
        );
        let rpc_resources = RpcResourceConfig {
            max_response_slice_len: maximum_transforms,
            ..RpcResourceConfig::default()
        };
        let snapshot_descriptor = RpcDescriptor::for_fixed_slice_with_resources::<
            StaticSnapshotRequest,
            StaticSnapshot,
        >(snapshot_capability.rpc_service_name(), rpc_resources);

        // The concrete endpoints are fully opened before either role is staged.
        let mut provider = StaticTfProvider::open(
            &update_descriptor,
            &snapshot_descriptor,
            update_authority,
            snapshot_capability,
            initial,
            maximum_transforms,
        )?;
        let control = provider.control();
        let wake = provider.wake_handle();
        self.provide::<UpdateRole>(update_authority, update_descriptor);
        self.provide_rpc::<SnapshotRole>(snapshot_capability, snapshot_descriptor);
        self.scope().wake_on_shutdown(move || {
            let _ = wake.wake();
        });
        let task_wake = provider.wake_handle();
        let task = self.spawn_critical_blocking(
            "tf-static-snapshot-server",
            move |cancellation| serve_static_provider(&mut provider, cancellation),
            move || {
                let _ = task_wake.wake();
            },
        );
        Ok(ProvidedStaticTf {
            update_authority,
            snapshot_capability,
            control,
            task,
        })
    }

    fn provide_dynamic_tf<DynamicRole>(
        &mut self,
        profile: StreamProfile,
    ) -> TransportResult<ProvidedDynamicTf>
    where
        DynamicRole: Capability,
    {
        let capability = CapabilityInstanceId::new();
        let descriptor =
            Stream::<DynamicTransform>::descriptor(capability.stream_service_name(), profile);
        let publisher = DynamicTfPublisher::open(&descriptor, capability)?;
        let scoped_publisher = publisher.clone();
        self.scope().defer_sync(move || {
            let _ = scoped_publisher.deactivate();
        });
        self.provide::<DynamicRole>(capability, descriptor);
        Ok(ProvidedDynamicTf {
            capability,
            publisher,
        })
    }

    fn require_static_tf<UpdateRole, SnapshotRole>(
        &mut self,
        update_requirement: impl AsRef<str>,
        snapshot_requirement: impl AsRef<str>,
        slot: &str,
        maximum_transforms: usize,
        buffer: TransformBuffer,
    ) -> TransportResult<StaticTfBinding>
    where
        UpdateRole: Capability,
        SnapshotRole: Capability,
    {
        if UpdateRole::TYPE_NAME == SnapshotRole::TYPE_NAME {
            return Err(TransportError::PairingInvalid(
                "static update and snapshot must use distinct capability roles",
            ));
        }
        let update_binding = self.binding::<UpdateRole>(update_requirement)?;
        let snapshot_binding = self.binding::<SnapshotRole>(snapshot_requirement)?;
        validate_pairing(&update_binding, &snapshot_binding)?;
        let update_descriptor = update_binding
            .try_stream()
            .ok_or(TransportError::PairingInvalid(
                "update role is not a stream",
            ))?
            .clone();
        let snapshot_descriptor = snapshot_binding
            .try_rpc()
            .ok_or(TransportError::PairingInvalid("snapshot role is not RPC"))?
            .clone();
        let update_authority = update_binding.descriptor().capability_instance();
        let snapshot_capability = snapshot_binding.descriptor().capability_instance();

        let client = StaticTfClient::open(
            update_authority,
            &update_descriptor,
            &snapshot_descriptor,
            snapshot_capability,
            self.activation(),
            slot,
            maximum_transforms,
            buffer,
        )?;
        let authority = client.authority();
        let rpc_wake = client.wake_handle();
        let wait = WaitContext::new(self.process_instance(), self.activation())?;
        let wait_wake = wait.wake_handle();
        self.scope().wake_on_shutdown(move || {
            let _ = rpc_wake.expire();
        });
        self.scope().wake_on_shutdown({
            let wait_wake = wait_wake.clone();
            move || {
                let _ = wait_wake.wake();
            }
        });
        let task_wake = wait_wake.clone();
        let task = self.spawn_critical_blocking(
            "tf-static-update-ingestor",
            move |cancellation| maintain_static_client(client, wait, cancellation),
            move || {
                let _ = task_wake.wake();
            },
        );
        Ok(StaticTfBinding { authority, task })
    }

    fn require_dynamic_tf<DynamicRole>(
        &mut self,
        requirement: impl AsRef<str>,
        slot: &str,
        buffer: TransformBuffer,
    ) -> TransportResult<DynamicTfBinding>
    where
        DynamicRole: Capability,
    {
        let binding = self.binding::<DynamicRole>(requirement)?;
        if binding.provider != *binding.descriptor().provider_component() {
            return Err(TransportError::PairingInvalid(
                "dynamic binding provider does not match its descriptor",
            ));
        }
        let descriptor = binding
            .try_stream()
            .ok_or(TransportError::PairingInvalid(
                "dynamic role is not a stream",
            ))?
            .clone();
        let capability = binding.descriptor().capability_instance();
        let subscriber = DynamicTfSubscriber::open(&descriptor, capability, slot, buffer)?;
        let authority = subscriber.authority();
        let wait = WaitContext::new(self.process_instance(), self.activation())?;
        let wait_wake = wait.wake_handle();
        self.scope().wake_on_shutdown({
            let wait_wake = wait_wake.clone();
            move || {
                let _ = wait_wake.wake();
            }
        });
        let task_wake = wait_wake.clone();
        let task = self.spawn_critical_blocking(
            "tf-dynamic-stream-ingestor",
            move |cancellation| ingest_dynamic_stream(subscriber, wait, cancellation),
            move || {
                let _ = task_wake.wake();
            },
        );
        Ok(DynamicTfBinding { authority, task })
    }
}

fn validate_pairing<UpdateRole: Capability, SnapshotRole: Capability>(
    update: &rbt_core::Binding<UpdateRole>,
    snapshot: &rbt_core::Binding<SnapshotRole>,
) -> TransportResult<()> {
    let update_descriptor = update.descriptor();
    let snapshot_descriptor = snapshot.descriptor();
    if update.provider != *update_descriptor.provider_component()
        || snapshot.provider != *snapshot_descriptor.provider_component()
        || update_descriptor.provider_component() != snapshot_descriptor.provider_component()
        || update_descriptor.provider_process() != snapshot_descriptor.provider_process()
        || update_descriptor.activation() != snapshot_descriptor.activation()
    {
        return Err(TransportError::PairingInvalid(
            "update and snapshot endpoints must belong to the same provider process activation",
        ));
    }
    Ok(())
}

fn serve_static_provider(
    provider: &mut StaticTfProvider,
    cancellation: CancellationToken,
) -> TransportResult<()> {
    while !cancellation.is_cancelled() {
        match provider.serve_one(Duration::from_millis(250)) {
            Ok(_) => {}
            Err(TransportError::Rpc(rbt_iox2::RpcError::ActivationExpired))
                if cancellation.is_cancelled() =>
            {
                break;
            }
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

fn maintain_static_client(
    client: StaticTfClient,
    mut wait: WaitContext,
    cancellation: CancellationToken,
) -> TransportResult<()> {
    let source = client.event_source(WaitToken::new(1));
    let sources = [source];
    while !cancellation.is_cancelled() {
        let result = wait.run_with_timeout(&sources, Duration::from_millis(250), |_, budget| {
            let report = client.drain_updates(budget)?;
            Ok::<_, TransportError>(WaitProgress::samples(report.received))
        })?;
        if result.is_some() || cancellation.is_cancelled() || wait.is_cancelled() {
            return Ok(());
        }
        let _ = client.refresh_if_due(Duration::from_millis(250));
    }
    Ok(())
}

fn ingest_dynamic_stream(
    subscriber: DynamicTfSubscriber,
    mut wait: WaitContext,
    cancellation: CancellationToken,
) -> TransportResult<()> {
    let source = subscriber.event_source(WaitToken::new(1));
    let sources = [source];
    while !cancellation.is_cancelled() {
        let result = wait.run(&sources, |_, budget| {
            let report = subscriber.drain_bounded(budget)?;
            Ok::<_, TransportError>(WaitProgress::samples(report.received))
        })?;
        if cancellation.is_cancelled() || wait.is_cancelled() || result.is_cancelled() {
            return Ok(());
        }
    }
    Ok(())
}

use std::{
    ops::DerefMut,
    sync::{Arc, Mutex},
    time::Duration,
};

use rbt_core::{ActivationId, CapabilityInstanceId, StreamPressure};
use rbt_iox2::{
    FixedSliceRpcClient, FixedSliceRpcServer, RpcServerWakeHandle, SlicePublisher, SliceStream,
    SliceSubscriber, StreamDescriptor, WaitToken,
};
use rbt_tf::{Authority, StaticSet, TfError, TfResourceConfig, TransformBuffer, wire};
use rbt_types::{StaticSnapshot, StaticSnapshotRequest, StaticUpdate};

use crate::{DrainReport, TransportError, TransportResult};

const STATUS_SUCCESS: u32 = 0;
const STATUS_SNAPSHOT_TOO_LARGE: u32 = 1;
const STATUS_PROVIDER_UNAVAILABLE: u32 = 2;

struct StaticProviderInner {
    publisher: Mutex<Option<SlicePublisher<StaticUpdate>>>,
    current: Mutex<StaticSet>,
    update_authority: CapabilityInstanceId,
    maximum_transforms: usize,
}

/// A control handle for replacing one provider's complete static set.
#[derive(Clone)]
pub struct StaticTfProviderControl {
    inner: Arc<StaticProviderInner>,
}

impl std::fmt::Debug for StaticTfProviderControl {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let state = match self.inner.publisher.lock() {
            Ok(publisher) if publisher.is_some() => "active",
            Ok(_) => "ended",
            Err(_) => "poisoned",
        };
        formatter
            .debug_struct("StaticTfProviderControl")
            .field("authority", &self.inner.update_authority)
            .field("maximum_transforms", &self.inner.maximum_transforms)
            .field("state", &state)
            .finish_non_exhaustive()
    }
}

impl StaticTfProviderControl {
    /// Returns the physical update-stream identity used as snapshot authority.
    pub fn authority(&self) -> CapabilityInstanceId {
        self.inner.update_authority
    }

    /// Returns a copy of the current bounded full set.
    pub fn current(&self) -> TransportResult<StaticSet> {
        self.inner
            .current
            .lock()
            .map(|current| current.clone())
            .map_err(|_| TransportError::LockPoisoned("static provider set"))
    }

    /// Returns the native update publisher's pressure snapshot while active.
    pub fn update_pressure_snapshot(&self) -> TransportResult<StreamPressure> {
        let publisher = self
            .inner
            .publisher
            .lock()
            .map_err(|_| TransportError::LockPoisoned("static publisher"))?;
        publisher
            .as_ref()
            .map(SlicePublisher::pressure)
            .ok_or(TransportError::ActivationEnded)
    }

    /// Atomically replaces the set and publishes one complete update revision.
    pub fn replace(&self, replacement: StaticSet) -> TransportResult<()> {
        validate_static_set(&replacement, self.inner.maximum_transforms)?;
        // Hold both locks through send and state replacement so a control
        // handle cannot publish after deactivation and concurrent revisions
        // cannot be reordered on the native stream.
        let publisher = self
            .inner
            .publisher
            .lock()
            .map_err(|_| TransportError::LockPoisoned("static publisher"))?;
        let publisher = publisher.as_ref().ok_or(TransportError::ActivationEnded)?;
        let mut current = self
            .inner
            .current
            .lock()
            .map_err(|_| TransportError::LockPoisoned("static provider set"))?;
        let expected_revision = current
            .revision
            .checked_add(1)
            .ok_or(TransportError::InvalidStaticReplacement)?;
        if replacement.revision != expected_revision
            || replacement.effective_from.clock() != current.effective_from.clock()
            || replacement.effective_from.nanoseconds() < current.effective_from.nanoseconds()
        {
            return Err(TransportError::InvalidStaticReplacement);
        }

        let mut sample = publisher.loan_slice(replacement.transforms.len())?;
        *sample.header_mut() =
            wire::static_update_header(self.inner.update_authority, &replacement);
        for (output, edge) in sample.payload_mut().iter_mut().zip(&replacement.transforms) {
            *output = wire::encode_static_edge(edge);
        }

        // Keep replacement and publication serialized, so concurrent updates
        // cannot put revision n+1 on the stream before revision n.
        sample.send()?;
        *current = replacement;
        Ok(())
    }

    pub(crate) fn deactivate(&self) -> TransportResult<()> {
        match self.inner.publisher.lock() {
            Ok(mut publisher) => {
                drop(publisher.take());
                Ok(())
            }
            Err(poisoned) => {
                // Teardown still releases the native endpoint, while making
                // the poisoned shared state explicit to direct callers.
                drop(poisoned.into_inner().take());
                Err(TransportError::LockPoisoned("static publisher"))
            }
        }
    }
}

/// Result of serving one native static-snapshot RPC request.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StaticServeOutcome {
    /// No request arrived before the bounded receive timeout.
    NoRequest,
    /// A response was sent with the contained wire status.
    Served { status: u32 },
}

/// Provider for a full static snapshot RPC and full-set update stream.
pub struct StaticTfProvider {
    inner: StaticTfProviderControl,
    server: FixedSliceRpcServer<StaticSnapshotRequest, StaticSnapshot>,
}

impl std::fmt::Debug for StaticTfProvider {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("StaticTfProvider")
            .field("control", &self.inner)
            .finish_non_exhaustive()
    }
}

impl StaticTfProvider {
    /// Opens native endpoints after validating the whole initial static set.
    pub fn open(
        update_descriptor: &StreamDescriptor,
        snapshot_descriptor: &rbt_types::RpcDescriptor,
        update_authority: CapabilityInstanceId,
        snapshot_capability: CapabilityInstanceId,
        initial: StaticSet,
        maximum_transforms: usize,
    ) -> TransportResult<Self> {
        validate_identity(
            update_descriptor.service_name(),
            update_authority.stream_service_name(),
        )?;
        validate_identity(
            snapshot_descriptor.service_name(),
            snapshot_capability.rpc_service_name(),
        )?;
        validate_identity_pair(update_authority, snapshot_capability)?;
        validate_static_set(&initial, maximum_transforms)?;
        if snapshot_descriptor.resources().max_response_slice_len < maximum_transforms {
            return Err(TransportError::PairingInvalid(
                "snapshot RPC response capacity is smaller than the provider bound",
            ));
        }

        let publisher =
            SliceStream::<StaticUpdate>::publisher(update_descriptor, maximum_transforms)?;
        let server = FixedSliceRpcServer::<StaticSnapshotRequest, StaticSnapshot>::open_or_create_for_capability(
            snapshot_descriptor,
            snapshot_capability,
        )?;
        Ok(Self {
            inner: StaticTfProviderControl {
                inner: Arc::new(StaticProviderInner {
                    publisher: Mutex::new(Some(publisher)),
                    current: Mutex::new(initial),
                    update_authority,
                    maximum_transforms,
                }),
            },
            server,
        })
    }

    /// Returns the provider's update authority.
    pub fn authority(&self) -> CapabilityInstanceId {
        self.inner.authority()
    }

    /// Returns a clonable application handle for replacing the current set.
    pub fn control(&self) -> StaticTfProviderControl {
        self.inner.clone()
    }

    /// Returns a wake handle used to stop an activation-owned server loop.
    pub fn wake_handle(&self) -> RpcServerWakeHandle {
        self.server.wake_handle()
    }

    /// Returns the native snapshot RPC server's pressure snapshot.
    pub fn snapshot_rpc_pressure(&self) -> rbt_core::RpcPressure {
        self.server.pressure_snapshot()
    }

    /// Replaces the full static set and publishes its update.
    pub fn replace(&self, replacement: StaticSet) -> TransportResult<()> {
        self.inner.replace(replacement)
    }

    /// Serves at most one request, with a native bounded receive timeout.
    pub fn serve_one(&mut self, timeout: Duration) -> TransportResult<StaticServeOutcome> {
        let Some(request) = self.server.receive_timeout(timeout)? else {
            return Ok(StaticServeOutcome::NoRequest);
        };

        let current = self.inner.current()?;
        let requested_authority_matches =
            request.authority == *self.inner.inner.update_authority.as_uuid().as_bytes();
        let malformed = request.reserved.iter().any(|byte| *byte != 0);
        let (status, count) = if !requested_authority_matches || malformed {
            (STATUS_PROVIDER_UNAVAILABLE, 0)
        } else if usize::try_from(request.max_transforms).unwrap_or(usize::MAX)
            < current.transforms.len()
        {
            (STATUS_SNAPSHOT_TOO_LARGE, 0)
        } else {
            (STATUS_SUCCESS, current.transforms.len())
        };

        let mut header = wire::static_snapshot_header(self.inner.inner.update_authority, &current);
        header.status = status;
        if !requested_authority_matches {
            // Echo the request's authority so a correctly paired client can
            // decode the explicit ProviderUnavailable status.
            header.authority = request.authority;
        }
        header.count = if status == STATUS_SUCCESS {
            u32::try_from(count).map_err(|_| {
                TransportError::PairingInvalid("static set count exceeds the wire limit")
            })?
        } else {
            0
        };
        let mut response = request.loan_response_slice(count)?;
        *response.user_header_mut() = header;
        if status == STATUS_SUCCESS {
            for (output, edge) in response.deref_mut().iter_mut().zip(&current.transforms) {
                *output = wire::encode_static_edge(edge);
            }
        }
        response.send()?;
        Ok(StaticServeOutcome::Served { status })
    }
}

impl Drop for StaticTfProvider {
    fn drop(&mut self) {
        let _ = self.inner.deactivate();
    }
}

/// Client for static updates plus bounded, single-flight snapshot resynchronization.
pub struct StaticTfClient {
    updates: SliceSubscriber<StaticUpdate>,
    snapshots: FixedSliceRpcClient<StaticSnapshotRequest, StaticSnapshot>,
    physical_authority: CapabilityInstanceId,
    authority: Authority,
    buffer: TransformBuffer,
    maximum_transforms: usize,
    snapshot_lock: Mutex<()>,
}

impl std::fmt::Debug for StaticTfClient {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("StaticTfClient")
            .field("authority", &self.authority)
            .field("physical_authority", &self.physical_authority)
            .field("maximum_transforms", &self.maximum_transforms)
            .finish_non_exhaustive()
    }
}

impl StaticTfClient {
    /// Opens the update listener before the RPC client and binds its authority token.
    // A client pairs two native endpoints and identities with its activation,
    // slot, edge bound, and buffer. Keep the existing public constructor shape.
    #[allow(clippy::too_many_arguments)]
    pub fn open(
        expected_authority: CapabilityInstanceId,
        update_descriptor: &StreamDescriptor,
        snapshot_descriptor: &rbt_types::RpcDescriptor,
        snapshot_capability: CapabilityInstanceId,
        activation: ActivationId,
        slot: &str,
        maximum_transforms: usize,
        buffer: TransformBuffer,
    ) -> TransportResult<Self> {
        validate_identity(
            update_descriptor.service_name(),
            expected_authority.stream_service_name(),
        )?;
        validate_identity(
            snapshot_descriptor.service_name(),
            snapshot_capability.rpc_service_name(),
        )?;
        validate_identity_pair(expected_authority, snapshot_capability)?;
        if maximum_transforms == 0
            || maximum_transforms > 4096
            || snapshot_descriptor.resources().max_response_slice_len < maximum_transforms
        {
            return Err(TransportError::PairingInvalid(
                "snapshot transform bound does not fit the RPC descriptor",
            ));
        }

        // subscriber() creates its native readiness listener before exposing
        // the payload port. Do not issue the first snapshot request until this
        // construction has completed.
        let updates = SliceStream::<StaticUpdate>::subscriber(update_descriptor)?;
        let snapshots =
            FixedSliceRpcClient::<StaticSnapshotRequest, StaticSnapshot>::open_for_activation(
                snapshot_descriptor,
                snapshot_capability,
                activation,
            )?;
        let authority = buffer.bind_provider(slot, expected_authority)?;
        Ok(Self {
            updates,
            snapshots,
            physical_authority: expected_authority,
            authority,
            buffer,
            maximum_transforms,
            snapshot_lock: Mutex::new(()),
        })
    }

    /// Returns the current buffer binding token.
    pub const fn authority(&self) -> Authority {
        self.authority
    }

    /// Returns a wake handle that expires any pending snapshot call.
    pub fn wake_handle(&self) -> rbt_iox2::RpcClientWakeHandle {
        self.snapshots.wake_handle()
    }

    /// Creates the readiness source for an application-owned WaitContext.
    pub fn event_source(&self, token: WaitToken) -> rbt_iox2::EventSource<'_> {
        self.updates.event_source(token)
    }

    /// Returns the native update subscriber's pressure snapshot.
    pub fn update_pressure_snapshot(&self) -> StreamPressure {
        self.updates.pressure()
    }

    /// Returns the native snapshot RPC client's pressure snapshot.
    pub fn snapshot_rpc_pressure(&self) -> rbt_core::RpcPressure {
        self.snapshots.pressure_snapshot()
    }

    /// Drains and applies at most 64 complete full-set updates.
    pub fn drain_updates(&self, budget: usize) -> TransportResult<DrainReport> {
        if budget > rbt_iox2::MAX_DRAIN_PER_SOURCE {
            return Err(TransportError::DrainBudget {
                requested: budget,
                maximum: rbt_iox2::MAX_DRAIN_PER_SOURCE,
            });
        }
        let mut report = DrainReport::default();
        while report.received < budget {
            let sample = match self.updates.receive() {
                Ok(Some(sample)) => sample,
                Ok(None) => break,
                Err(error) => {
                    self.mark_snapshot_stale();
                    return Err(error.into());
                }
            };
            report.received += 1;
            let update = wire::decode_static_update(
                self.physical_authority,
                sample.header(),
                sample.payload(),
                self.maximum_transforms,
            );
            drop(sample);
            match update.and_then(|update| self.buffer.apply_static_update(&self.authority, update))
            {
                Ok(()) => report.accepted += 1,
                Err(error) => {
                    let _ = self.buffer.mark_snapshot_stale(&self.authority);
                    report.rejected += 1;
                    report.failures.push(error);
                }
            }
        }
        Ok(report)
    }

    /// Forces one bounded snapshot request and applies its complete response.
    pub fn refresh_snapshot(&self, timeout: Duration) -> TransportResult<()> {
        let _single_flight = match self.snapshot_lock.try_lock() {
            Ok(lock) => lock,
            Err(std::sync::TryLockError::Poisoned(_)) => {
                self.mark_snapshot_stale();
                return Err(TransportError::LockPoisoned("snapshot request"));
            }
            Err(std::sync::TryLockError::WouldBlock) => {
                return Err(TransportError::SnapshotInFlight);
            }
        };
        let result = (|| {
            let request = StaticSnapshotRequest {
                authority: *self.physical_authority.as_uuid().as_bytes(),
                max_transforms: u32::try_from(self.maximum_transforms).map_err(|_| {
                    TransportError::PairingInvalid("snapshot bound exceeds the wire limit")
                })?,
                reserved: [0; 4],
            };
            let snapshot = {
                let mut request_loan = self.snapshots.loan_request()?;
                *request_loan = request;
                let response = request_loan.send()?.wait(timeout)?;
                wire::decode_static_snapshot(
                    self.physical_authority,
                    response.user_header(),
                    response.payload(),
                    self.maximum_transforms,
                )?
            };
            // The listener existed before the request. Merge full updates queued
            // during the RPC before exposing its potentially older snapshot.
            // No response guard is held while draining or mutating local history.
            let drained = self.drain_updates(rbt_iox2::MAX_DRAIN_PER_SOURCE)?;
            if let Some(error) = drained
                .failures
                .into_iter()
                .find(|error| !matches!(error, TfError::SnapshotRevisionGap))
            {
                return Err(TransportError::Tf(error));
            }
            self.buffer
                .apply_static_snapshot(&self.authority, snapshot)?;
            Ok(())
        })();
        if result.is_err() {
            self.mark_snapshot_stale();
        }
        result
    }

    fn mark_snapshot_stale(&self) {
        let _ = self.buffer.mark_snapshot_stale(&self.authority);
    }

    /// Requests a snapshot only when the buffer's steady-clock resync policy says it is due.
    pub fn refresh_if_due(&self, timeout: Duration) -> TransportResult<bool> {
        if !self.buffer.needs_resync(&self.authority)? {
            return Ok(false);
        }
        self.refresh_snapshot(timeout)?;
        Ok(true)
    }
}

impl Drop for StaticTfClient {
    fn drop(&mut self) {
        let _ = self.buffer.unbind_provider(&self.authority);
    }
}

fn validate_identity(actual: &str, expected: String) -> TransportResult<()> {
    if actual == expected {
        Ok(())
    } else {
        Err(TransportError::ServiceIdentityMismatch)
    }
}

fn validate_identity_pair(
    update_authority: CapabilityInstanceId,
    snapshot_capability: CapabilityInstanceId,
) -> TransportResult<()> {
    if update_authority.as_uuid().is_nil()
        || snapshot_capability.as_uuid().is_nil()
        || update_authority == snapshot_capability
    {
        return Err(TransportError::PairingInvalid("capability identity is nil"));
    }
    Ok(())
}

fn validate_static_set(set: &StaticSet, maximum_transforms: usize) -> TransportResult<()> {
    if maximum_transforms == 0
        || maximum_transforms > 4096
        || set.transforms.len() > maximum_transforms
    {
        return Err(TransportError::PairingInvalid(
            "static set is outside the configured transform bound",
        ));
    }
    let resources = TfResourceConfig {
        max_providers: 1,
        max_pending_static_updates: 1,
        max_total_edges: maximum_transforms,
        max_static_transforms_per_provider: maximum_transforms,
        ..TfResourceConfig::default()
    };
    let validator = TransformBuffer::new(resources, rbt_tf::LookupPolicy::default())?;
    let authority = validator.bind_provider("tf_validation", CapabilityInstanceId::new())?;
    validator.apply_static_snapshot(&authority, set.clone())?;
    Ok(())
}

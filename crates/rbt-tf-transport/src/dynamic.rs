use std::sync::{Arc, Mutex};

use rbt_core::{CapabilityInstanceId, StreamPressure};
use rbt_iox2::{MAX_DRAIN_PER_SOURCE, Publisher, Stream, StreamDescriptor, Subscriber, WaitToken};
use rbt_tf::{Authority, DynamicRecord, TfError, TransformBuffer, wire};
use rbt_types::DynamicTransform;

use crate::{TransportError, TransportResult};

struct DynamicPublisherInner {
    publisher: Mutex<Option<Publisher<DynamicTransform>>>,
    authority: CapabilityInstanceId,
}

/// A native dynamic-TF publisher tied to one physical capability identity.
#[derive(Clone)]
pub struct DynamicTfPublisher {
    inner: Arc<DynamicPublisherInner>,
}

impl std::fmt::Debug for DynamicTfPublisher {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let active = match self.inner.publisher.lock() {
            Ok(publisher) if publisher.is_some() => "active",
            Ok(_) => "ended",
            Err(_) => "poisoned",
        };
        formatter
            .debug_struct("DynamicTfPublisher")
            .field("authority", &self.inner.authority)
            .field("state", &active)
            .finish_non_exhaustive()
    }
}

impl DynamicTfPublisher {
    /// Opens a native publisher after checking the endpoint identity.
    pub fn open(
        descriptor: &StreamDescriptor,
        authority: CapabilityInstanceId,
    ) -> TransportResult<Self> {
        if authority.as_uuid().is_nil() {
            return Err(TfError::InvalidConfig.into());
        }
        if descriptor.service_name() != authority.stream_service_name() {
            return Err(TransportError::ServiceIdentityMismatch);
        }
        let publisher = Stream::<DynamicTransform>::publisher(descriptor)?;
        Ok(Self {
            inner: Arc::new(DynamicPublisherInner {
                publisher: Mutex::new(Some(publisher)),
                authority,
            }),
        })
    }

    /// Returns the physical authority written into every native sample.
    pub fn authority(&self) -> CapabilityInstanceId {
        self.inner.authority
    }

    /// Returns the publisher's native stream pressure snapshot while active.
    pub fn pressure_snapshot(&self) -> TransportResult<StreamPressure> {
        let publisher = self
            .inner
            .publisher
            .lock()
            .map_err(|_| TransportError::LockPoisoned("dynamic publisher"))?;
        publisher
            .as_ref()
            .map(Publisher::pressure)
            .ok_or(TransportError::ActivationEnded)
    }

    /// Encodes and sends one complete native dynamic transform.
    pub fn publish(&self, record: &DynamicRecord) -> TransportResult<usize> {
        if record.parent == record.child {
            return Err(TfError::CycleDetected.into());
        }
        let publisher = self
            .inner
            .publisher
            .lock()
            .map_err(|_| TransportError::LockPoisoned("dynamic publisher"))?;
        let publisher = publisher.as_ref().ok_or(TransportError::ActivationEnded)?;
        let mut loan = publisher.loan()?;
        *loan = wire::encode_dynamic(self.inner.authority, record);
        Ok(loan.send()?)
    }

    pub(crate) fn deactivate(&self) -> TransportResult<()> {
        match self.inner.publisher.lock() {
            Ok(mut publisher) => {
                drop(publisher.take());
                Ok(())
            }
            Err(poisoned) => {
                // Endpoint release is still required during activation teardown.
                drop(poisoned.into_inner().take());
                Err(TransportError::LockPoisoned("dynamic publisher"))
            }
        }
    }
}

/// Counters and bounded payload failures from one stream drain.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DrainReport {
    /// Number of native samples received and guards released.
    pub received: usize,
    /// Number of records accepted by the local buffer.
    pub accepted: usize,
    /// Number of records rejected by wire or buffer validation.
    pub rejected: usize,
    /// Typed failures, with at most one entry per received sample.
    pub failures: Vec<TfError>,
}

/// A bounded native subscriber that decodes and copies dynamic TF into a buffer.
pub struct DynamicTfSubscriber {
    subscriber: Subscriber<DynamicTransform>,
    authority: Authority,
    physical_authority: CapabilityInstanceId,
    buffer: TransformBuffer,
}

impl std::fmt::Debug for DynamicTfSubscriber {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("DynamicTfSubscriber")
            .field("authority", &self.authority)
            .field("physical_authority", &self.physical_authority)
            .finish_non_exhaustive()
    }
}

impl DynamicTfSubscriber {
    /// Opens a native subscriber and binds its physical authority to a logical slot.
    pub fn open(
        descriptor: &StreamDescriptor,
        expected_authority: CapabilityInstanceId,
        slot: &str,
        buffer: TransformBuffer,
    ) -> TransportResult<Self> {
        if descriptor.service_name() != expected_authority.stream_service_name() {
            return Err(TransportError::ServiceIdentityMismatch);
        }
        let subscriber = Stream::<DynamicTransform>::subscriber(descriptor)?;
        let authority = buffer.bind_provider(slot, expected_authority)?;
        Ok(Self {
            subscriber,
            authority,
            physical_authority: expected_authority,
            buffer,
        })
    }

    /// Returns the buffer authority token created for this binding.
    pub const fn authority(&self) -> Authority {
        self.authority
    }

    /// Creates the readiness source for an application-owned WaitContext.
    pub fn event_source(&self, token: WaitToken) -> rbt_iox2::EventSource<'_> {
        self.subscriber.event_source(token)
    }

    /// Returns the subscriber's native stream pressure snapshot.
    pub fn pressure_snapshot(&self) -> StreamPressure {
        self.subscriber.pressure()
    }

    /// Receives at most 64 samples, validates them, copies accepted records, and drops each guard.
    pub fn drain_bounded(&self, budget: usize) -> TransportResult<DrainReport> {
        if budget > MAX_DRAIN_PER_SOURCE {
            return Err(TransportError::DrainBudget {
                requested: budget,
                maximum: MAX_DRAIN_PER_SOURCE,
            });
        }

        let mut report = DrainReport::default();
        while report.received < budget {
            let Some(sample) = self.subscriber.receive()? else {
                break;
            };
            report.received += 1;
            let record = wire::decode_dynamic(self.physical_authority, &sample);
            drop(sample);
            match record.and_then(|record| self.buffer.ingest_dynamic(&self.authority, record)) {
                Ok(()) => report.accepted += 1,
                Err(error) => {
                    report.rejected += 1;
                    report.failures.push(error);
                }
            }
        }
        Ok(report)
    }
}

impl Drop for DynamicTfSubscriber {
    fn drop(&mut self) {
        let _ = self.buffer.unbind_provider(&self.authority);
    }
}

use std::{
    collections::VecDeque,
    sync::{Arc, Condvar, Mutex},
};

use rbt_types::StreamProfile;
use thiserror::Error;

/// One already-serialized message owned by the writer queue.
#[derive(Debug)]
pub struct QueueItem {
    /// MCAP channel ID assigned during writer setup.
    pub channel_id: u16,
    /// Original message sequence used for the MCAP sequence hint.
    pub sequence: u64,
    /// Monotonic-derived MCAP recording timestamp.
    pub log_time: u64,
    /// Stream profile controlling overload semantics.
    pub profile: StreamProfile,
    /// Canonical file payload buffer.
    pub data: Vec<u8>,
}

impl QueueItem {
    /// Bytes charged against the configured queue budget.
    pub fn charged_bytes(&self) -> usize {
        self.data.len()
    }
}

/// Nonblocking producer-side queue failures.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
pub enum QueueError {
    /// Adding the item would exceed the byte budget.
    #[error("writer queue full: item={item_bytes} used={used_bytes} budget={budget_bytes}")]
    Full {
        /// Serialized bytes in this item.
        item_bytes: usize,
        /// Bytes currently queued.
        used_bytes: usize,
        /// Configured maximum queued bytes.
        budget_bytes: usize,
    },
    /// The producer has been closed normally.
    #[error("writer queue is closed")]
    Closed,
    /// The writer encountered a permanent failure.
    #[error("writer failed: {0}")]
    WriterFailed(String),
    /// A queue thread panicked while holding its state lock.
    #[error("writer queue state is poisoned")]
    Poisoned,
}

#[derive(Debug)]
struct QueueState {
    items: VecDeque<QueueItem>,
    used_bytes: usize,
    high_water_bytes: usize,
    closed: bool,
    writer_error: Option<String>,
}

#[derive(Debug)]
struct SharedQueue {
    budget_bytes: usize,
    state: Mutex<QueueState>,
    available: Condvar,
}

/// Cloneable nonblocking producer for a byte-budgeted writer queue.
#[derive(Clone, Debug)]
pub struct ByteQueue {
    inner: Arc<SharedQueue>,
}

/// Single blocking consumer for a [`ByteQueue`].
#[derive(Debug)]
pub struct ByteQueueConsumer {
    inner: Arc<SharedQueue>,
}

impl ByteQueue {
    /// Creates a queue pair with an exact serialized-byte budget.
    pub fn new(budget_bytes: usize) -> (Self, ByteQueueConsumer) {
        let inner = Arc::new(SharedQueue {
            budget_bytes,
            state: Mutex::new(QueueState {
                items: VecDeque::new(),
                used_bytes: 0,
                high_water_bytes: 0,
                closed: false,
                writer_error: None,
            }),
            available: Condvar::new(),
        });
        (
            Self {
                inner: Arc::clone(&inner),
            },
            ByteQueueConsumer { inner },
        )
    }

    /// Attempts to enqueue without blocking a robot publisher or subscriber.
    pub fn try_push(&self, item: QueueItem) -> Result<(), QueueError> {
        let item_bytes = item.charged_bytes();
        let mut state = self.inner.state.lock().map_err(|_| QueueError::Poisoned)?;
        if let Some(error) = &state.writer_error {
            return Err(QueueError::WriterFailed(error.clone()));
        }
        if state.closed {
            return Err(QueueError::Closed);
        }
        let Some(next_used) = state.used_bytes.checked_add(item_bytes) else {
            return Err(QueueError::Full {
                item_bytes,
                used_bytes: state.used_bytes,
                budget_bytes: self.inner.budget_bytes,
            });
        };
        if next_used > self.inner.budget_bytes {
            return Err(QueueError::Full {
                item_bytes,
                used_bytes: state.used_bytes,
                budget_bytes: self.inner.budget_bytes,
            });
        }
        state.items.push_back(item);
        state.used_bytes = next_used;
        state.high_water_bytes = state.high_water_bytes.max(next_used);
        drop(state);
        self.inner.available.notify_one();
        Ok(())
    }

    /// Stops accepting new items. The consumer still drains queued items.
    pub fn close(&self) -> Result<(), QueueError> {
        let mut state = self.inner.state.lock().map_err(|_| QueueError::Poisoned)?;
        state.closed = true;
        drop(state);
        self.inner.available.notify_all();
        Ok(())
    }

    /// Returns `(current_bytes, high_water_bytes, budget_bytes)`.
    pub fn byte_usage(&self) -> Result<(usize, usize, usize), QueueError> {
        let state = self.inner.state.lock().map_err(|_| QueueError::Poisoned)?;
        Ok((
            state.used_bytes,
            state.high_water_bytes,
            self.inner.budget_bytes,
        ))
    }
}

impl ByteQueueConsumer {
    /// Blocks until one item is available or the closed queue is drained.
    pub fn pop(&self) -> Result<Option<QueueItem>, QueueError> {
        let mut state = self.inner.state.lock().map_err(|_| QueueError::Poisoned)?;
        loop {
            if let Some(item) = state.items.pop_front() {
                state.used_bytes = state.used_bytes.saturating_sub(item.charged_bytes());
                return Ok(Some(item));
            }
            if state.closed {
                return Ok(None);
            }
            state = self
                .inner
                .available
                .wait(state)
                .map_err(|_| QueueError::Poisoned)?;
        }
    }

    /// Publishes a permanent writer failure to all producers and closes input.
    pub fn fail(&self, error: impl Into<String>) -> Result<(), QueueError> {
        let mut state = self.inner.state.lock().map_err(|_| QueueError::Poisoned)?;
        state.writer_error = Some(error.into());
        state.closed = true;
        drop(state);
        self.inner.available.notify_all();
        Ok(())
    }

    /// Returns `(current_bytes, high_water_bytes, budget_bytes)`.
    pub fn byte_usage(&self) -> Result<(usize, usize, usize), QueueError> {
        let state = self.inner.state.lock().map_err(|_| QueueError::Poisoned)?;
        Ok((
            state.used_bytes,
            state.high_water_bytes,
            self.inner.budget_bytes,
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn item(bytes: usize) -> QueueItem {
        QueueItem {
            channel_id: 1,
            sequence: 1,
            log_time: 2,
            profile: StreamProfile::Sensor,
            data: vec![0; bytes],
        }
    }

    #[test]
    fn byte_budget_is_exact_and_draining_releases_it() {
        let (queue, consumer) = ByteQueue::new(5);
        queue.try_push(item(3)).unwrap();
        assert!(matches!(
            queue.try_push(item(3)),
            Err(QueueError::Full {
                used_bytes: 3,
                budget_bytes: 5,
                ..
            })
        ));
        assert_eq!(consumer.pop().unwrap().unwrap().data.len(), 3);
        queue.try_push(item(5)).unwrap();
        assert_eq!(queue.byte_usage().unwrap(), (5, 5, 5));
    }

    #[test]
    fn close_drains_and_writer_failure_reaches_producer() {
        let (queue, consumer) = ByteQueue::new(8);
        queue.try_push(item(1)).unwrap();
        queue.close().unwrap();
        assert!(consumer.pop().unwrap().is_some());
        assert!(consumer.pop().unwrap().is_none());

        let (queue, consumer) = ByteQueue::new(8);
        consumer.fail("disk").unwrap();
        assert_eq!(
            queue.try_push(item(1)),
            Err(QueueError::WriterFailed("disk".to_owned()))
        );
    }
}

//! A stream endpoint's iceoryx2 node must remove its node directory on drop,
//! including when a received or loaned sample outlives the endpoint.
//!
//! iceoryx2 cannot remove a node directory while ports of that node still
//! exist. Samples keep their port alive, so a node dropped before its samples
//! left an empty directory below the iceoryx2 root on every such drop. Each
//! node creation scans that directory, so the leftovers slowed every later
//! process on the host.
//!
//! Each case counts the entries of the default iceoryx2 node directory before
//! and after it runs, holding `SERIAL` so a sibling case's live node cannot be
//! counted. The counts are taken after every endpoint is gone; the assertion
//! allows no net growth.

use std::{
    fs,
    path::Path,
    sync::{
        Mutex, MutexGuard,
        atomic::{AtomicU64, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

use rbt_iox2::{Image, PixelFormat, SliceStream, StreamProfile};

const NODE_DIR: &str = "/tmp/iceoryx2/nodes";

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

// Cargo runs the cases of one binary on parallel threads.
static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
    SERIAL
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn unique_service_name() -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    let value = (u64::from(std::process::id()) << 16) | (sequence & 0xffff);
    format!("rbt/cap/00000000-0000-4000-8000-{value:012x}/node-cleanup")
}

/// Counts node directories owned by this process' (now dropped) nodes plus
/// any others; only the difference between two calls is meaningful.
fn node_dir_entries() -> usize {
    if !Path::new(NODE_DIR).exists() {
        return 0;
    }
    fs::read_dir(NODE_DIR)
        .unwrap_or_else(|error| panic!("failed to read {NODE_DIR}: {error}"))
        .filter_map(Result::ok)
        .filter(|entry| entry.file_type().is_ok_and(|kind| kind.is_dir()))
        .count()
}

fn publish_and_receive() -> (
    rbt_iox2::SliceSubscriber<Image>,
    rbt_iox2::SlicePublisher<Image>,
    rbt_iox2::ReceivedSliceSample<Image>,
) {
    let descriptor = SliceStream::<Image>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, 16).expect("image publisher");
    let subscriber = SliceStream::<Image>::subscriber(&descriptor).expect("image subscriber");
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let mut sample = publisher.loan_slice(16).expect("image loan");
        {
            let header = sample.header_mut();
            header.sequence = 1;
            header.width = 4;
            header.height = 4;
            header.stride = 4;
            header.encoding = PixelFormat::GRAY8;
        }
        sample.payload_mut().fill(7);
        sample.send().expect("image send");
        if let Some(received) = subscriber.receive().expect("image receive") {
            return (subscriber, publisher, received);
        }
        assert!(Instant::now() < deadline, "image sample was not delivered");
        thread::sleep(Duration::from_millis(1));
    }
}

#[test]
fn received_sample_outliving_its_subscriber_does_not_leak_node_directory() {
    let _serial = serial();
    let before = node_dir_entries();
    let (subscriber, publisher, received) = publish_and_receive();
    drop(subscriber);
    drop(publisher);
    assert_eq!(received.payload()[0], 7);
    drop(received);
    let after = node_dir_entries();
    assert!(
        after <= before,
        "node directory grew from {before} to {after} entries"
    );
}

#[test]
fn loaned_sample_outliving_its_publisher_does_not_leak_node_directory() {
    let _serial = serial();
    let before = node_dir_entries();
    let descriptor = SliceStream::<Image>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, 16).expect("image publisher");
    let loaned = publisher.loan_slice(16).expect("image loan");
    drop(publisher);
    drop(loaned);
    let after = node_dir_entries();
    assert!(
        after <= before,
        "node directory grew from {before} to {after} entries"
    );
}

//! End-to-end checks for the Phase 3 local message ABI.
//!
//! These tests deliberately exercise the public `rbt-iox2` wrappers rather
//! than reaching into iceoryx2 internals.  A service name contains both the
//! test process id and a monotonically increasing suffix so tests can run in
//! parallel without sharing a transport endpoint.

use std::{
    mem::{align_of, size_of},
    sync::atomic::{AtomicU64, Ordering},
    thread,
    time::{Duration, Instant},
};

use iceoryx2::prelude::ZeroCopySend;
use rbt_iox2::{
    ControlCommand, Image, ImageHeader, LayoutHash, MessageShape, MessageTypeDescriptor,
    PixelFormat, PointCloud, PointCloudHeader, PointXYZI, ShmSafe, SliceMessageAbi, SliceStream,
    Stream, StreamDescriptor, StreamError, StreamProfile,
};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name() -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    // The last UUID group is 48 bits.  The pid and atomic suffix make names
    // unique among concurrently-running test binaries on this host.
    let value = (u64::from(std::process::id()) << 16) | (sequence & 0xffff);
    format!("rbt/cap/00000000-0000-4000-8000-{value:012x}/stream")
}

fn receive_image(
    subscriber: &rbt_iox2::SliceSubscriber<Image>,
    timeout: Duration,
) -> rbt_iox2::Result<rbt_iox2::ReceivedSliceSample<Image>> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(sample) = subscriber.receive()? {
            return Ok(sample);
        }
        if Instant::now() >= deadline {
            panic!("timed out waiting for image sample");
        }
        thread::sleep(Duration::from_millis(1));
    }
}

fn image_byte(sequence: u64, index: usize) -> u8 {
    sequence.wrapping_add(index as u64) as u8
}

fn image_checksum(sequence: u64, payload_len: usize) -> u64 {
    (0..payload_len).fold(0_u64, |checksum, index| {
        checksum.wrapping_add(u64::from(image_byte(sequence, index)))
    })
}

fn write_image_frame(
    publisher: &rbt_iox2::SlicePublisher<Image>,
    sequence: u64,
    payload_len: usize,
) -> rbt_iox2::Result<usize> {
    let mut sample = publisher.loan_slice(payload_len)?;
    {
        let header = sample.header_mut();
        header.sequence = sequence;
        header.timestamp_ns = sequence * 1_000_000;
        header.width = 1920;
        header.height = 1080;
        header.stride = 1920 * 3;
        header.encoding = PixelFormat::RGB8;
    }
    for (index, byte) in sample.payload_mut().iter_mut().enumerate() {
        *byte = image_byte(sequence, index);
    }
    sample.send()
}

fn point_at(index: usize) -> PointXYZI {
    let value = index as f32;
    PointXYZI {
        x: value,
        y: value * 2.0,
        z: -value,
        intensity: (index % 31) as f32,
    }
}

fn point_checksum(point: PointXYZI) -> u64 {
    u64::from(point.x.to_bits())
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.y.to_bits()))
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.z.to_bits()))
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.intensity.to_bits()))
}

fn points_checksum(points: &[PointXYZI]) -> u64 {
    points.iter().copied().fold(0_u64, |checksum, point| {
        checksum.wrapping_add(point_checksum(point))
    })
}

fn expected_points_checksum(count: usize) -> u64 {
    (0..count).fold(0_u64, |checksum, index| {
        checksum.wrapping_add(point_checksum(point_at(index)))
    })
}

fn write_pointcloud_frame(
    publisher: &rbt_iox2::SlicePublisher<PointCloud>,
    sequence: u64,
    count: usize,
) -> rbt_iox2::Result<usize> {
    let mut sample = publisher.loan_slice_uninit(count)?;
    {
        let header = sample.header_mut();
        header.sequence = sequence;
        header.timestamp_ns = sequence * 1_000_000;
        header.width = count as u32;
        header.height = 1;
        header.dense = 1;
        header.reserved = [0; 7];
    }
    let sample = sample.write_from_fn(point_at);
    sample.send()
}

#[test]
fn image_1080p_roundtrip_writes_and_reads_the_loaned_slice() -> rbt_iox2::Result<()> {
    const WIDTH: usize = 1920;
    const HEIGHT: usize = 1080;
    const CHANNELS: usize = 3;
    const PAYLOAD_LEN: usize = WIDTH * HEIGHT * CHANNELS;

    let descriptor = SliceStream::<Image>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, PAYLOAD_LEN)?;
    let subscriber = SliceStream::<Image>::subscriber(&descriptor)?;

    for sequence in 0..3_u64 {
        // Service discovery is asynchronous.  A zero return means no
        // subscriber was connected yet; retrying keeps this test independent
        // of scheduling while still sending the exact same loaned payload.
        loop {
            if write_image_frame(&publisher, sequence, PAYLOAD_LEN)? > 0 {
                break;
            }
            thread::sleep(Duration::from_millis(1));
        }

        let sample = receive_image(&subscriber, Duration::from_secs(2))?;
        let header = sample.header();
        assert_eq!(header.sequence, sequence);
        assert_eq!(header.timestamp_ns, sequence * 1_000_000);
        assert_eq!(header.width, WIDTH as u32);
        assert_eq!(header.height, HEIGHT as u32);
        assert_eq!(header.stride, (WIDTH * CHANNELS) as u32);
        assert_eq!(header.encoding, PixelFormat::RGB8);

        let payload = sample.payload();
        assert_eq!(payload.len(), PAYLOAD_LEN);
        let checksum = payload.iter().map(|byte| u64::from(*byte)).sum::<u64>();
        assert_eq!(checksum, image_checksum(sequence, PAYLOAD_LEN));
        for &index in &[0, 1, 2, PAYLOAD_LEN / 2, PAYLOAD_LEN - 1] {
            assert_eq!(payload[index], image_byte(sequence, index));
        }
        eprintln!(
            "[image-test] sequence={sequence} payload={} checksum={checksum}",
            payload.len()
        );
    }

    eprintln!(
        "[image-test] resolution={WIDTH}x{HEIGHT} encoding=RGB8 payload={PAYLOAD_LEN} receive_count=3"
    );
    Ok(())
}

#[test]
fn pointcloud_roundtrip_supports_zero_and_large_dynamic_slices() -> rbt_iox2::Result<()> {
    const COUNTS: [usize; 4] = [0, 1, 100, 100_000];

    let descriptor =
        SliceStream::<PointCloud>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<PointCloud>::publisher(&descriptor, 100_000)?;
    let subscriber = SliceStream::<PointCloud>::subscriber(&descriptor)?;

    for (sequence, &count) in COUNTS.iter().enumerate() {
        loop {
            if write_pointcloud_frame(&publisher, sequence as u64, count)? > 0 {
                break;
            }
            thread::sleep(Duration::from_millis(1));
        }

        let deadline = Instant::now() + Duration::from_secs(2);
        let sample = loop {
            if let Some(sample) = subscriber.receive()? {
                break sample;
            }
            if Instant::now() >= deadline {
                panic!("timed out waiting for point-cloud sample of length {count}");
            }
            thread::sleep(Duration::from_millis(1));
        };
        assert_eq!(sample.header().sequence, sequence as u64);
        assert_eq!(sample.header().width, count as u32);
        assert_eq!(sample.header().height, 1);
        assert_eq!(sample.header().dense, 1);
        let payload = sample.payload();
        assert_eq!(payload.len(), count);
        assert_eq!(points_checksum(payload), expected_points_checksum(count));
        if let Some(first) = payload.first() {
            let expected = point_at(0);
            assert_eq!(first.x, expected.x);
            assert_eq!(first.y, expected.y);
            assert_eq!(first.z, expected.z);
            assert_eq!(first.intensity, expected.intensity);
        }
        if let Some(last) = payload.last() {
            let expected = point_at(count - 1);
            assert_eq!(last.x, expected.x);
            assert_eq!(last.y, expected.y);
            assert_eq!(last.z, expected.z);
            assert_eq!(last.intensity, expected.intensity);
        }
        eprintln!(
            "[pointcloud-test] sequence={sequence} elements={count} checksum={}",
            points_checksum(payload)
        );
    }
    Ok(())
}

#[test]
fn sensor_profile_replaces_stale_samples_for_a_slow_consumer() -> rbt_iox2::Result<()> {
    const ELEMENTS: usize = 64;
    const PUBLISH_COUNT: u64 = 90;
    const PUBLISH_PERIOD: Duration = Duration::from_millis(33);

    let descriptor =
        SliceStream::<PointCloud>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<PointCloud>::publisher(&descriptor, ELEMENTS)?;
    let subscriber = SliceStream::<PointCloud>::subscriber(&descriptor)?;

    let producer = thread::spawn(move || {
        let mut successful_sends = 0_u64;
        let mut delivered_sends = 0_u64;
        let start = Instant::now();
        for sequence in 0..PUBLISH_COUNT {
            let delivered = write_pointcloud_frame(&publisher, sequence, ELEMENTS)
                .expect("Sensor producer should not fail when a consumer is slow");
            successful_sends += 1;
            delivered_sends += u64::from(delivered > 0);

            let target = start + PUBLISH_PERIOD.saturating_mul((sequence + 1) as u32);
            if let Some(remaining) = target.checked_duration_since(Instant::now()) {
                thread::sleep(remaining);
            }
        }
        (successful_sends, delivered_sends)
    });

    let deadline = Instant::now() + Duration::from_secs(4);
    let mut received_sequences = Vec::new();
    while Instant::now() < deadline {
        if let Some(sample) = subscriber.receive()? {
            received_sequences.push(sample.header().sequence);
            assert_eq!(sample.payload().len(), ELEMENTS);
            drop(sample);
            // Five samples per second, while the producer runs at roughly
            // thirty samples per second.
            thread::sleep(Duration::from_millis(200));
        } else {
            thread::sleep(Duration::from_millis(1));
        }
    }

    let (successful_sends, delivered_sends) = producer.join().expect("producer thread");
    let estimated_drops: u64 = received_sequences
        .windows(2)
        .map(|pair| pair[1].saturating_sub(pair[0] + 1))
        .sum();
    let sequence_gaps = received_sequences
        .windows(2)
        .filter(|pair| pair[1] > pair[0] + 1)
        .count();

    assert_eq!(successful_sends, PUBLISH_COUNT);
    assert!(
        delivered_sends > 0,
        "subscriber never connected to producer"
    );
    assert!(
        received_sequences.len() >= 5,
        "slow consumer received too few samples"
    );
    assert!(
        estimated_drops > 0,
        "Sensor queue did not expose any sequence gap"
    );
    eprintln!(
        "[sensor-slow-test] sent={successful_sends} received={} sequence_gaps={sequence_gaps} estimated_dropped={estimated_drops}",
        received_sequences.len(),
    );
    Ok(())
}

#[test]
fn control_profile_reports_queue_saturation_instead_of_overwriting() -> rbt_iox2::Result<()> {
    let descriptor =
        Stream::<ControlCommand>::descriptor(unique_service_name(), StreamProfile::Control);
    let publisher = Stream::<ControlCommand>::publisher(&descriptor)?;
    let subscriber = Stream::<ControlCommand>::subscriber(&descriptor)?;

    let mut next_sequence = 0_u64;
    let mut connected = false;
    let mut sent = 0_u64;
    let mut delivered = 0_u64;
    let mut backpressure_events = 0_u64;
    let mut failed_delivery = 0_u64;
    for _ in 0..100 {
        let mut sample = publisher.loan()?;
        sample.sequence = next_sequence;
        sample.value = next_sequence as f32;
        sample.reserved = 0;
        next_sequence += 1;
        sent += 1;
        if sample.send()? > 0 {
            delivered += 1;
            connected = true;
            break;
        }
        thread::sleep(Duration::from_millis(1));
    }
    assert!(connected, "control subscriber never connected");

    let mut queue_full_sequence = None;
    for _ in 0..32 {
        let sequence = next_sequence;
        let mut sample = publisher.loan()?;
        sample.sequence = sequence;
        sample.value = sequence as f32;
        sample.reserved = 0;
        next_sequence += 1;
        sent += 1;
        match sample.send() {
            Ok(recipients) => {
                delivered += u64::from(recipients > 0);
                eprintln!("[control-test] sequence={sequence} recipients={recipients}");
            }
            Err(StreamError::QueueFull) => {
                backpressure_events += 1;
                failed_delivery += 1;
                queue_full_sequence = Some(sequence);
                eprintln!("[control-test] queue saturation sequence={sequence} error=QueueFull");
                break;
            }
            Err(error) => return Err(error),
        }
    }

    assert!(
        queue_full_sequence.is_some(),
        "Control publisher silently accepted all commands while subscriber was not reading"
    );
    eprintln!(
        "[control-test] sent={sent} delivered={delivered} backpressure_events={backpressure_events} failed_delivery={failed_delivery}"
    );
    // Keep the subscriber alive until after the failed send.  Dropping it
    // earlier would make a non-delivery look like successful queueing.
    drop(subscriber);
    Ok(())
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct ImageHeaderV2 {
    sequence: u64,
    timestamp_ns: u64,
    width: u32,
    height: u32,
    stride: u32,
    encoding: PixelFormat,
    exposure_us: u32,
}

// SAFETY: This test-only header is repr(C), Copy, owns no addresses, and all
// fields are shared-memory-safe scalar values or transparent newtypes.
unsafe impl ShmSafe for ImageHeaderV2 {}
// SAFETY: The representation is repr(C) and contains only ZeroCopySend fields.
unsafe impl ZeroCopySend for ImageHeaderV2 {}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct ImageV2;

// SAFETY: The test-only marker is repr(C), zero-sized, and has no state.
unsafe impl ShmSafe for ImageV2 {}
// SAFETY: The test-only marker carries no process-local state.
unsafe impl ZeroCopySend for ImageV2 {}

impl SliceMessageAbi for ImageV2 {
    type Header = ImageHeaderV2;
    type Element = u8;

    const NAMESPACE: &'static str = "rbt.sensor";
    const NAME: &'static str = "Image";
    const ABI_MAJOR: u16 = 1;
    const SIGNATURE: &'static str = "header(sequence:u64@0,timestamp_ns:u64@8,width:u32@16,height:u32@20,stride:u32@24,encoding:PixelFormat@28,exposure_us:u32@32,tail_padding:[u8;4]@36);element:u8@0";
}

#[test]
fn abi_mismatch_is_rejected_before_transport_open_and_reports_both_hashes() {
    let descriptor = StreamDescriptor::new(
        unique_service_name(),
        Image::descriptor(),
        StreamProfile::Sensor,
    );
    let expected = ImageV2::descriptor();
    let provider = descriptor.message_type().clone();
    assert_eq!(expected.namespace, provider.namespace);
    assert_eq!(expected.name, provider.name);
    assert_eq!(expected.abi_major, provider.abi_major);
    assert_ne!(expected.layout_hash, provider.layout_hash);

    let error = SliceStream::<ImageV2>::subscriber(&descriptor)
        .expect_err("a changed layout must be rejected before opening the service");
    match error {
        StreamError::AbiMismatch {
            message,
            expected_layout,
            actual_layout,
            expected_abi_major,
            actual_abi_major,
            ..
        } => {
            assert_eq!(message, "rbt.sensor/Image@1");
            assert_eq!(expected_layout, expected.layout_hash);
            assert_eq!(actual_layout, provider.layout_hash);
            assert_eq!(expected_abi_major, 1);
            assert_eq!(actual_abi_major, 1);
            eprintln!(
                "binding rejected: message={message} expected_layout={expected_layout} provider_layout={actual_layout} expected_abi={expected_abi_major} provider_abi={actual_abi_major}"
            );
        }
        other => panic!("expected AbiMismatch, got {other:?}"),
    }
}

#[test]
fn slice_loan_rejects_lengths_above_the_configured_maximum() -> rbt_iox2::Result<()> {
    let descriptor = SliceStream::<Image>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, 2)?;
    assert!(matches!(
        publisher.loan_slice(3),
        Err(StreamError::InvalidPayloadLength {
            requested: 3,
            maximum: 2
        })
    ));
    Ok(())
}

#[test]
fn built_in_message_layouts_are_explicit_and_stable_on_this_target() {
    assert_eq!(size_of::<ImageHeader>(), 32);
    assert_eq!(align_of::<ImageHeader>(), 8);
    assert_eq!(size_of::<PointCloudHeader>(), 32);
    assert_eq!(align_of::<PointCloudHeader>(), 8);
    assert_eq!(size_of::<PointXYZI>(), 16);
    assert_eq!(align_of::<PointXYZI>(), 4);
    assert_eq!(size_of::<ControlCommand>(), 16);
    assert_eq!(align_of::<ControlCommand>(), 8);

    assert_eq!(Image::descriptor().shape, MessageShape::Slice);
    assert_eq!(PointCloud::descriptor().shape, MessageShape::Slice);
    assert_eq!(ControlCommand::descriptor().shape, MessageShape::Fixed);
    assert_eq!(
        Image::descriptor().layout_hash,
        LayoutHash::from_hex("943fa4008cf0f55f89a281b051c69a6f").unwrap()
    );
    assert_eq!(
        PointCloud::descriptor().layout_hash,
        LayoutHash::from_hex("d9da2bb29bb2cf217c40f1a56d1fb755").unwrap()
    );
    assert_eq!(
        ControlCommand::descriptor().layout_hash,
        LayoutHash::from_hex("9381cf15842a48ac0964111f0f45ef72").unwrap()
    );

    let _descriptor_type_check: MessageTypeDescriptor = Image::descriptor();
}

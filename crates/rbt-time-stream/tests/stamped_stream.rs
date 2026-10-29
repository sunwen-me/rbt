use std::{
    sync::atomic::{AtomicU64, Ordering},
    thread,
    time::{Duration, Instant},
};

use rbt_iox2::{Image, SliceStream, StreamError, StreamProfile};
use rbt_time::{ClockIdentity, ClockKind, ClockSourceId, SensorTiming, Timestamp};
use rbt_time_stream::{StampedBytes, TIMING_PREFIX_LEN, encode_timing_into, split_payload};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name() -> String {
    let value = (u64::from(std::process::id()) << 16)
        | (NEXT_SERVICE.fetch_add(1, Ordering::Relaxed) & 0xffff);
    format!("rbt/cap/00000000-0000-4000-8000-{value:012x}/stream")
}

fn monotonic(nanoseconds: i64) -> Timestamp {
    let mut source = [0_u8; 48];
    source[..16].copy_from_slice(&[7; 16]);
    source[16..24].copy_from_slice(&1_u64.to_le_bytes());
    source[24..32].copy_from_slice(&2_u64.to_le_bytes());
    source[32..40].copy_from_slice(&3_i64.to_le_bytes());
    source[40..48].copy_from_slice(&4_i64.to_le_bytes());
    Timestamp::new(
        ClockIdentity::new(
            ClockKind::HostMonotonic,
            ClockSourceId::from_bytes(source).unwrap(),
            5,
        )
        .unwrap(),
        nanoseconds,
    )
}

#[test]
fn stamped_payload_uses_one_stream_loan_and_borrow_for_timing_and_bytes() -> rbt_iox2::Result<()> {
    let descriptor =
        SliceStream::<StampedBytes>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<StampedBytes>::publisher(&descriptor, TIMING_PREFIX_LEN + 5)?;
    let subscriber = SliceStream::<StampedBytes>::subscriber(&descriptor)?;
    let timing = SensorTiming::new(None, monotonic(20), None, Some(3)).unwrap();
    let deadline = Instant::now() + Duration::from_secs(2);

    loop {
        let mut loan = publisher.loan_slice(TIMING_PREFIX_LEN + 5)?;
        *loan.header_mut() = 42;
        encode_timing_into(&timing, &mut loan.payload_mut()[..TIMING_PREFIX_LEN]).unwrap();
        loan.payload_mut()[TIMING_PREFIX_LEN..].copy_from_slice(b"hello");
        if loan.send()? > 0 {
            break;
        }
        assert!(Instant::now() < deadline, "subscriber did not connect");
        thread::sleep(Duration::from_millis(1));
    }

    let received = loop {
        if let Some(sample) = subscriber.receive()? {
            break sample;
        }
        assert!(
            Instant::now() < deadline,
            "timed out receiving stamped bytes"
        );
        thread::sleep(Duration::from_millis(1));
    };
    assert_eq!(*received.header(), 42);
    let (decoded, application) = split_payload(received.payload()).unwrap();
    assert_eq!(decoded, timing);
    assert_eq!(application, b"hello");
    Ok(())
}

#[test]
fn legacy_image_descriptor_cannot_open_as_stamped_bytes() {
    let descriptor = SliceStream::<Image>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let error = SliceStream::<StampedBytes>::subscriber(&descriptor)
        .expect_err("Image identity must never be interpreted as StampedBytes");
    assert!(matches!(error, StreamError::AbiMismatch { .. }));
}

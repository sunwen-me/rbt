//! End-to-end native zero-copy RPC checks for the three dynamic message shapes.

use std::{
    sync::atomic::{AtomicU64, Ordering},
    thread,
    time::Duration,
};

use rbt_iox2::{
    FixedSliceRpcClient, FixedSliceRpcServer, Frame, Image, PointCloud, PointCloudHeader,
    PointXYZI, RpcDescriptor, RpcError, RpcResourceConfig, SliceFixedRpcClient,
    SliceFixedRpcServer, SliceRpcClient, SliceRpcServer,
};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name(shape: &str) -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    format!(
        "rbt/rpc/00000000-0000-4000-8000-{:012x}/dynamic-{shape}",
        (u64::from(std::process::id()) << 16) | sequence
    )
}

fn resources(max_request_slice_len: usize, max_response_slice_len: usize) -> RpcResourceConfig {
    RpcResourceConfig::new(
        1,
        1,
        1,
        1,
        1,
        1,
        max_request_slice_len,
        max_response_slice_len,
    )
}

const SMALL_LENGTHS: [usize; 4] = [0, 1, 100, 100_000];
const ONE_MIB: usize = 1024 * 1024;
const TWENTY_MIB: usize = 20 * 1024 * 1024;
const MAX_POINT_COUNT: usize = 100_000;

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

fn expected_points_checksum(count: usize) -> u64 {
    (0..count).fold(0_u64, |checksum, index| {
        checksum.wrapping_add(point_checksum(point_at(index)))
    })
}

fn assert_pointcloud_header(header: &PointCloudHeader, sequence: u64, count: usize) {
    assert_eq!(header.sequence, sequence);
    assert_eq!(header.timestamp_ns, sequence * 1_000_000);
    assert_eq!(header.width, count as u32);
    assert_eq!(header.height, 1);
    assert_eq!(header.dense, 1);
    assert_eq!(header.reserved, [0; 7]);
}

fn assert_pointcloud_payload(payload: &[PointXYZI], count: usize) {
    assert_eq!(payload.len(), count);
    for (index, actual) in payload.iter().copied().enumerate() {
        let expected = point_at(index);
        assert_eq!(actual.x, expected.x, "x mismatch at point {index}");
        assert_eq!(actual.y, expected.y, "y mismatch at point {index}");
        assert_eq!(actual.z, expected.z, "z mismatch at point {index}");
        assert_eq!(
            actual.intensity, expected.intensity,
            "intensity mismatch at point {index}"
        );
    }
    let checksum = payload.iter().copied().fold(0_u64, |checksum, point| {
        checksum.wrapping_add(point_checksum(point))
    });
    assert_eq!(checksum, expected_points_checksum(count));
}

#[test]
fn fixed_request_slice_response_is_bounded_zero_copy() -> Result<(), RpcError> {
    let descriptor = RpcDescriptor::for_fixed_slice_with_resources::<Frame, Image>(
        unique_service_name("fixed-slice"),
        resources(1, TWENTY_MIB),
    );
    let mut server = FixedSliceRpcServer::<Frame, Image>::open_or_create(&descriptor)?;
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for expected_len in SMALL_LENGTHS.into_iter().chain([TWENTY_MIB]) {
            let request = server
                .receive_timeout(Duration::from_secs(5))?
                .expect("fixed request");
            assert_eq!(request.sequence as usize, expected_len);
            let mut response = request.loan_response_slice(expected_len)?;
            response.fill((expected_len & 0xff) as u8);
            response.send()?;
        }
        Ok(())
    });

    let client = FixedSliceRpcClient::<Frame, Image>::open(&descriptor)?;
    for expected_len in SMALL_LENGTHS.into_iter().chain([TWENTY_MIB]) {
        let mut request = client.loan_request()?;
        request.sequence = expected_len as u64;
        let response = request.send()?.wait(Duration::from_secs(10))?;
        assert_eq!(response.len(), expected_len);
        assert!(
            response
                .iter()
                .all(|value| *value == (expected_len & 0xff) as u8)
        );
    }

    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn slice_request_fixed_response_is_bounded_zero_copy() -> Result<(), RpcError> {
    let descriptor = RpcDescriptor::for_slice_fixed_with_resources::<Image, Frame>(
        unique_service_name("slice-fixed"),
        resources(ONE_MIB, 1),
    );
    let mut server = SliceFixedRpcServer::<Image, Frame>::open_or_create(&descriptor)?;
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for expected_len in SMALL_LENGTHS.into_iter().chain([ONE_MIB]) {
            let request = server
                .receive_timeout(Duration::from_secs(5))?
                .expect("slice request");
            assert_eq!(request.len(), expected_len);
            assert!(
                request
                    .iter()
                    .all(|value| *value == (expected_len & 0xff) as u8)
            );
            let mut response = request.loan_response()?;
            response.sequence = expected_len as u64;
            response.value = 0x5a;
            response.send()?;
        }
        Ok(())
    });

    let client = SliceFixedRpcClient::<Image, Frame>::open(&descriptor)?;
    for expected_len in SMALL_LENGTHS.into_iter().chain([ONE_MIB]) {
        let mut request = client.loan_request_slice(expected_len)?;
        request.fill((expected_len & 0xff) as u8);
        let response = request.send()?.wait(Duration::from_secs(10))?;
        assert_eq!(response.sequence, expected_len as u64);
        assert_eq!(response.value, 0x5a);
    }

    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn slice_request_slice_response_is_bounded_zero_copy() -> Result<(), RpcError> {
    let descriptor = RpcDescriptor::for_slice_with_resources::<Image, Image>(
        unique_service_name("slice-slice"),
        resources(ONE_MIB, TWENTY_MIB),
    );
    let mut server = SliceRpcServer::<Image, Image>::open_or_create(&descriptor)?;
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for (expected_request_len, expected_response_len) in SMALL_LENGTHS
            .into_iter()
            .map(|length| (length, length))
            .chain([(ONE_MIB, TWENTY_MIB)])
        {
            let request = server
                .receive_timeout(Duration::from_secs(5))?
                .expect("slice request");
            assert_eq!(request.len(), expected_request_len);
            assert!(
                request
                    .iter()
                    .all(|value| *value == (expected_request_len & 0xff) as u8)
            );
            let mut response = request.loan_response_slice(expected_response_len)?;
            response.fill((expected_response_len & 0xff) as u8);
            response.send()?;
        }
        Ok(())
    });

    let client = SliceRpcClient::<Image, Image>::open(&descriptor)?;
    for (expected_request_len, expected_response_len) in SMALL_LENGTHS
        .into_iter()
        .map(|length| (length, length))
        .chain([(ONE_MIB, TWENTY_MIB)])
    {
        let mut request = client.loan_request_slice(expected_request_len)?;
        request.fill((expected_request_len & 0xff) as u8);
        let response = request.send()?.wait(Duration::from_secs(15))?;
        assert_eq!(response.len(), expected_response_len);
        assert!(
            response
                .iter()
                .all(|value| *value == (expected_response_len & 0xff) as u8)
        );
    }

    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn pointcloud_slice_request_slice_response_is_bounded_zero_copy() -> Result<(), RpcError> {
    let descriptor = RpcDescriptor::for_slice_with_resources::<PointCloud, PointCloud>(
        unique_service_name("pointcloud-slice-slice"),
        resources(MAX_POINT_COUNT, MAX_POINT_COUNT),
    );
    let mut server = SliceRpcServer::<PointCloud, PointCloud>::open_or_create(&descriptor)?;
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for (ordinal, expected_len) in SMALL_LENGTHS.into_iter().enumerate() {
            let request = server
                .receive_timeout(Duration::from_secs(5))?
                .expect("pointcloud request");
            assert_eq!(request.len(), expected_len);
            assert_pointcloud_header(request.user_header(), ordinal as u64, expected_len);
            assert_pointcloud_payload(&request, expected_len);

            let response_len = expected_len;
            let mut response = request.loan_response_slice(response_len)?;
            let response_sequence = 1_000 + ordinal as u64;
            *response.user_header_mut() = PointCloudHeader {
                sequence: response_sequence,
                timestamp_ns: response_sequence * 1_000_000,
                width: response_len as u32,
                height: 1,
                dense: 1,
                reserved: [0; 7],
            };
            for (index, point) in response.iter_mut().enumerate() {
                *point = point_at(index);
            }
            response.send()?;
        }

        let request = server
            .receive_timeout(Duration::from_secs(5))?
            .expect("pointcloud bounds request");
        assert_eq!(request.len(), 0);
        let error = match request.loan_response_slice(MAX_POINT_COUNT + 1) {
            Ok(_) => panic!("response slice beyond configured maximum unexpectedly succeeded"),
            Err(error) => error,
        };
        match error {
            RpcError::InvalidSliceLength {
                direction,
                requested,
                maximum,
            } => {
                assert_eq!(direction, "response");
                assert_eq!(requested, MAX_POINT_COUNT + 1);
                assert_eq!(maximum, MAX_POINT_COUNT);
            }
            other => panic!("unexpected response bound error: {other:?}"),
        }
        Ok(())
    });

    let client = SliceRpcClient::<PointCloud, PointCloud>::open(&descriptor)?;
    let error = match client.loan_request_slice(MAX_POINT_COUNT + 1) {
        Ok(_) => panic!("request slice beyond configured maximum unexpectedly succeeded"),
        Err(error) => error,
    };
    match error {
        RpcError::InvalidSliceLength {
            direction,
            requested,
            maximum,
        } => {
            assert_eq!(direction, "request");
            assert_eq!(requested, MAX_POINT_COUNT + 1);
            assert_eq!(maximum, MAX_POINT_COUNT);
        }
        other => panic!("unexpected request bound error: {other:?}"),
    }

    for (ordinal, expected_len) in SMALL_LENGTHS.into_iter().enumerate() {
        let mut request = client.loan_request_slice(expected_len)?;
        *request.user_header_mut() = PointCloudHeader {
            sequence: ordinal as u64,
            timestamp_ns: ordinal as u64 * 1_000_000,
            width: expected_len as u32,
            height: 1,
            dense: 1,
            reserved: [0; 7],
        };
        for (index, point) in request.iter_mut().enumerate() {
            *point = point_at(index);
        }

        let response = request.send()?.wait(Duration::from_secs(15))?;
        let response_sequence = 1_000 + ordinal as u64;
        assert_pointcloud_header(response.user_header(), response_sequence, expected_len);
        assert_pointcloud_payload(&response, expected_len);
    }

    let request = client.loan_request_slice(0)?;
    let pending = request.send()?;
    server_thread.join().expect("server thread")?;
    drop(pending);
    Ok(())
}

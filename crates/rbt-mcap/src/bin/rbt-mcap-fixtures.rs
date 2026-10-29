use std::{
    collections::BTreeMap,
    env, fs,
    path::{Path, PathBuf},
};

use rbt_mcap::{
    ChannelDefinition, ChannelProvenance, KnownMessageType, McapMessage, RecordingMetadata,
    RecordingWriter, encode_control, encode_image, encode_pointcloud, scan_recording,
    visit_messages,
};
use rbt_types::{
    ControlCommand, ImageHeader, MESSAGE_DESCRIPTOR_VERSION, PixelFormat, PointCloudHeader,
    PointXYZI, StreamProfile,
};

const FIXTURE_START_NS: u64 = 1_700_000_000_000_000_000;

#[derive(Clone)]
struct ExpectedMessage {
    channel: usize,
    log_time: u64,
    sequence: u64,
    data: Vec<u8>,
}

struct Fixture {
    name: &'static str,
    channels: Vec<ChannelDefinition>,
    messages: Vec<ExpectedMessage>,
}

fn main() {
    if let Err(error) = run() {
        eprintln!("rbt-mcap-fixtures: {error}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = env::args().skip(1);
    let operation = arguments.next().unwrap_or_else(|| "verify".to_owned());
    let directory = arguments
        .next()
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("tests/fixtures/mcap"));
    if arguments.next().is_some() {
        return Err(
            "usage: rbt-mcap-fixtures <generate|verify|generate-large|verify-large> [directory]"
                .into(),
        );
    }
    match operation.as_str() {
        "generate" => {
            let fixtures = fixtures();
            fs::create_dir_all(&directory)?;
            for fixture in &fixtures {
                write_fixture(&directory, fixture)?;
            }
            for fixture in &fixtures {
                verify_fixture(&directory, fixture)?;
            }
            println!("generated and verified {} MCAP fixtures", fixtures.len());
        }
        "verify" => {
            let fixtures = fixtures();
            for fixture in &fixtures {
                verify_fixture(&directory, fixture)?;
            }
            println!("verified {} MCAP fixtures", fixtures.len());
        }
        "generate-large" => {
            let fixtures = large_fixtures();
            fs::create_dir_all(&directory)?;
            for fixture in &fixtures {
                write_fixture(&directory, fixture)?;
            }
            for fixture in &fixtures {
                verify_fixture(&directory, fixture)?;
            }
            println!(
                "generated and verified {} large MCAP fixtures",
                fixtures.len()
            );
        }
        "verify-large" => {
            let fixtures = large_fixtures();
            for fixture in &fixtures {
                verify_fixture(&directory, fixture)?;
            }
            println!("verified {} large MCAP fixtures", fixtures.len());
        }
        _ => {
            return Err(
                "usage: rbt-mcap-fixtures <generate|verify|generate-large|verify-large> [directory]"
                    .into(),
            );
        }
    }
    Ok(())
}

fn metadata() -> RecordingMetadata {
    RecordingMetadata {
        rbt_sdk_version: "0.3.0-fixture".to_owned(),
        idl_language_version: 1,
        message_descriptor_version: MESSAGE_DESCRIPTOR_VERSION,
        control_protocol_version: 2,
        host_architecture: "fixture".to_owned(),
        endianness: "little".to_owned(),
        recording_start_time_ns: FIXTURE_START_NS,
    }
}

fn definition(kind: KnownMessageType, topic: &str, profile: StreamProfile) -> ChannelDefinition {
    ChannelDefinition {
        topic: topic.to_owned(),
        message_type: kind.descriptor().to_owned(),
        profile,
        capability_type: match kind {
            KnownMessageType::Image => "Camera",
            KnownMessageType::PointCloud => "Lidar",
            KnownMessageType::ControlCommand => "MotionCommand",
        }
        .to_owned(),
        capability_name: topic.trim_start_matches('/').replace('/', "-"),
        capability_qualifier: "fixture".to_owned(),
        provenance: ChannelProvenance {
            source_component: "fixture-generator".to_owned(),
            source_process_instance: "00000000-0000-0000-0000-000000000001".to_owned(),
            source_activation: "1".to_owned(),
            source_capability_instance: format!("00000000-0000-0000-0000-{:012}", kind as u8 + 2),
        },
    }
}

fn write_fixture(directory: &Path, fixture: &Fixture) -> Result<(), Box<dyn std::error::Error>> {
    let path = directory.join(fixture.name);
    let mut writer = RecordingWriter::create(&path, &fixture.channels, &metadata())?;
    for message in &fixture.messages {
        let channel_id = writer
            .channel_id(message.channel)
            .ok_or("fixture channel index is out of range")?;
        writer.write_message(
            channel_id,
            message.sequence,
            FIXTURE_START_NS + message.log_time,
            &message.data,
        )?;
    }
    writer.finish(BTreeMap::from([
        ("complete".to_owned(), "true".to_owned()),
        ("fixture".to_owned(), fixture.name.to_owned()),
        ("recorder_drops".to_owned(), "0".to_owned()),
        ("transport_sequence_gaps".to_owned(), "0".to_owned()),
        ("writer_errors".to_owned(), "0".to_owned()),
    ]))?;
    Ok(())
}

fn verify_fixture(directory: &Path, fixture: &Fixture) -> Result<(), Box<dyn std::error::Error>> {
    let path = directory.join(fixture.name);
    let catalog = scan_recording(&path, false)?;
    if catalog.channels.len() != fixture.channels.len()
        || catalog.message_count != fixture.messages.len() as u64
    {
        return Err(format!("{}: catalog count mismatch", fixture.name).into());
    }
    let mut observed = Vec::<(String, u64, Vec<u8>)>::new();
    visit_messages(
        &path,
        false,
        &mut |channel: &rbt_mcap::CatalogChannel, message: McapMessage<'_>| {
            observed.push((
                channel.topic.clone(),
                message.log_time,
                message.data.to_vec(),
            ));
            Ok(())
        },
    )?;
    let expected = fixture
        .messages
        .iter()
        .map(|message| {
            (
                fixture.channels[message.channel].topic.clone(),
                FIXTURE_START_NS + message.log_time,
                message.data.clone(),
            )
        })
        .collect::<Vec<_>>();
    if observed != expected {
        return Err(format!("{}: semantic payload/timing mismatch", fixture.name).into());
    }
    Ok(())
}

fn fixtures() -> Vec<Fixture> {
    let image_channel = definition(
        KnownMessageType::Image,
        "/fixture/camera",
        StreamProfile::Sensor,
    );
    let point_channel = definition(
        KnownMessageType::PointCloud,
        "/fixture/lidar",
        StreamProfile::Sensor,
    );
    let control_channel = definition(
        KnownMessageType::ControlCommand,
        "/fixture/motion-command",
        StreamProfile::Control,
    );

    let image_one = image(1, 11_000, 3);
    let image_two = image(2, 22_000, 97);
    let points_zero = pointcloud(10, 31_000, &[]);
    let points_one = pointcloud(11, 32_000, &[(1.0, -2.0, 3.5, 4.25)]);
    let points_four = pointcloud(
        12,
        33_000,
        &[
            (0.0, 0.5, 1.0, 1.5),
            (-2.0, -2.5, -3.0, -3.5),
            (10.0, 20.0, 30.0, 40.0),
            (f32::from_bits(0x7fc0_1234), -0.0, f32::INFINITY, 7.0),
        ],
    );
    let command_one = control(20, 0.25, 0x1020_3040);
    let command_two = control(21, -3.5, 0x5060_7080);
    let command_three = control(22, f32::from_bits(0x7fc0_4321), 0xaabb_ccdd);

    vec![
        Fixture {
            name: "image-small.mcap",
            channels: vec![image_channel.clone()],
            messages: vec![
                ExpectedMessage {
                    channel: 0,
                    log_time: 0,
                    sequence: 1,
                    data: image_one.clone(),
                },
                ExpectedMessage {
                    channel: 0,
                    log_time: 50_000_000,
                    sequence: 2,
                    data: image_two.clone(),
                },
            ],
        },
        Fixture {
            name: "pointcloud-small.mcap",
            channels: vec![point_channel.clone()],
            messages: vec![
                ExpectedMessage {
                    channel: 0,
                    log_time: 0,
                    sequence: 10,
                    data: points_zero.clone(),
                },
                ExpectedMessage {
                    channel: 0,
                    log_time: 25_000_000,
                    sequence: 11,
                    data: points_one.clone(),
                },
                ExpectedMessage {
                    channel: 0,
                    log_time: 75_000_000,
                    sequence: 12,
                    data: points_four.clone(),
                },
            ],
        },
        Fixture {
            name: "control-small.mcap",
            channels: vec![control_channel.clone()],
            messages: vec![
                ExpectedMessage {
                    channel: 0,
                    log_time: 0,
                    sequence: 20,
                    data: command_one.clone(),
                },
                ExpectedMessage {
                    channel: 0,
                    log_time: 10_000_000,
                    sequence: 21,
                    data: command_two.clone(),
                },
                ExpectedMessage {
                    channel: 0,
                    log_time: 20_000_000,
                    sequence: 22,
                    data: command_three.clone(),
                },
            ],
        },
        Fixture {
            name: "mixed-small.mcap",
            channels: vec![image_channel, point_channel, control_channel],
            messages: vec![
                ExpectedMessage {
                    channel: 0,
                    log_time: 0,
                    sequence: 1,
                    data: image_one,
                },
                ExpectedMessage {
                    channel: 2,
                    log_time: 20_000_000,
                    sequence: 20,
                    data: command_one,
                },
                ExpectedMessage {
                    channel: 1,
                    log_time: 50_000_000,
                    sequence: 12,
                    data: points_four,
                },
                ExpectedMessage {
                    channel: 2,
                    log_time: 50_000_000,
                    sequence: 21,
                    data: command_two,
                },
                ExpectedMessage {
                    channel: 0,
                    log_time: 100_000_000,
                    sequence: 2,
                    data: image_two,
                },
            ],
        },
    ]
}

fn large_fixtures() -> Vec<Fixture> {
    const IMAGE_BYTES: usize = 1920 * 1080 * 3;
    const POINTS_20_MIB: usize = (20 * 1024 * 1024) / std::mem::size_of::<PointXYZI>();

    let image_channel = definition(
        KnownMessageType::Image,
        "/generated/camera",
        StreamProfile::Sensor,
    );
    let point_channel = definition(
        KnownMessageType::PointCloud,
        "/generated/lidar",
        StreamProfile::Sensor,
    );
    let image_header = ImageHeader {
        sequence: 100,
        timestamp_ns: 100_000,
        width: 1920,
        height: 1080,
        stride: 1920 * 3,
        encoding: PixelFormat::RGB8,
    };
    let image_payload = (0..IMAGE_BYTES)
        .map(|index| (index as u8).wrapping_mul(31).wrapping_add(7))
        .collect::<Vec<_>>();

    vec![
        Fixture {
            name: "image-1920x1080-generated.mcap",
            channels: vec![image_channel],
            messages: vec![ExpectedMessage {
                channel: 0,
                log_time: 0,
                sequence: image_header.sequence,
                data: encode_image(&image_header, &image_payload),
            }],
        },
        Fixture {
            name: "pointcloud-100000-generated.mcap",
            channels: vec![point_channel.clone()],
            messages: vec![ExpectedMessage {
                channel: 0,
                log_time: 0,
                sequence: 101,
                data: generated_pointcloud(101, 100_000),
            }],
        },
        Fixture {
            name: "pointcloud-20mib-generated.mcap",
            channels: vec![point_channel],
            messages: vec![ExpectedMessage {
                channel: 0,
                log_time: 0,
                sequence: 102,
                data: generated_pointcloud(102, POINTS_20_MIB),
            }],
        },
    ]
}

fn generated_pointcloud(sequence: u64, count: usize) -> Vec<u8> {
    let header = PointCloudHeader {
        sequence,
        timestamp_ns: sequence * 1_000,
        width: count as u32,
        height: 1,
        dense: 1,
        reserved: [0; 7],
    };
    let points = (0..count)
        .map(|index| {
            let value = index as f32;
            PointXYZI {
                x: value,
                y: -value,
                z: value * 0.5,
                intensity: f32::from_bits((index as u32).wrapping_mul(2_654_435_761)),
            }
        })
        .collect::<Vec<_>>();
    encode_pointcloud(&header, &points)
}

fn image(sequence: u64, timestamp_ns: u64, seed: u8) -> Vec<u8> {
    let header = ImageHeader {
        sequence,
        timestamp_ns,
        width: 8,
        height: 4,
        stride: 24,
        encoding: PixelFormat::RGB8,
    };
    let pixels = (0_u8..96)
        .map(|index| index.wrapping_mul(17).wrapping_add(seed))
        .collect::<Vec<_>>();
    encode_image(&header, &pixels)
}

fn pointcloud(sequence: u64, timestamp_ns: u64, values: &[(f32, f32, f32, f32)]) -> Vec<u8> {
    let header = PointCloudHeader {
        sequence,
        timestamp_ns,
        width: values.len() as u32,
        height: 1,
        dense: 1,
        reserved: [0; 7],
    };
    let points = values
        .iter()
        .map(|&(x, y, z, intensity)| PointXYZI { x, y, z, intensity })
        .collect::<Vec<_>>();
    encode_pointcloud(&header, &points)
}

fn control(sequence: u64, value: f32, reserved: u32) -> Vec<u8> {
    encode_control(&ControlCommand {
        sequence,
        value,
        reserved,
    })
}

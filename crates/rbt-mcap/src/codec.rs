use rbt_types::{
    CanonicalFieldCodec, CanonicalFixedMessage, CanonicalSliceMessage, ControlCommand, Image,
    ImageHeader, PixelFormat, PointCloud, PointCloudHeader, PointXYZI,
};

use crate::{
    Error, KnownMessageType, MAX_IMAGE_PAYLOAD_BYTES, MAX_MESSAGE_BYTES, MAX_POINTCLOUD_POINTS,
    Result,
};

/// Encodes one Image sample with its generated canonical codec.
pub fn encode_image(header: &ImageHeader, payload: &[u8]) -> Vec<u8> {
    Image::encode_file(header, payload)
}

/// Encodes one PointCloud sample with its generated canonical codec.
pub fn encode_pointcloud(header: &PointCloudHeader, payload: &[PointXYZI]) -> Vec<u8> {
    PointCloud::encode_file(header, payload)
}

/// Encodes one ControlCommand with its generated canonical codec.
pub fn encode_control(command: &ControlCommand) -> Vec<u8> {
    command.encode_file()
}

/// Returns the original rbt header sequence without allocating.
pub fn payload_sequence(kind: KnownMessageType, data: &[u8]) -> Result<u64> {
    ensure_message_bound(data)?;
    let bytes = data.get(..8).ok_or_else(|| Error::CorruptRecord {
        channel: kind.schema_name(),
        reason: "payload does not contain a u64 sequence field".to_owned(),
    })?;
    let mut sequence = [0_u8; 8];
    sequence.copy_from_slice(bytes);
    Ok(u64::from_le_bytes(sequence))
}

/// Returns the encoded slice element count, or zero for a fixed message.
pub fn payload_element_count(kind: KnownMessageType, data: &[u8]) -> Result<usize> {
    ensure_message_bound(data)?;
    match kind {
        KnownMessageType::Image => Image::encoded_element_count(data).map_err(Into::into),
        KnownMessageType::PointCloud => PointCloud::encoded_element_count(data).map_err(Into::into),
        KnownMessageType::ControlCommand => Ok(0),
    }
}

/// Validates canonical bytes and all v0.3 per-type safety invariants.
pub fn validate_payload(kind: KnownMessageType, data: &[u8]) -> Result<()> {
    ensure_message_bound(data)?;
    match kind {
        KnownMessageType::Image => validate_image(data),
        KnownMessageType::PointCloud => validate_pointcloud(data),
        KnownMessageType::ControlCommand => {
            let mut command = ControlCommand::default();
            command.decode_file_into(data)?;
            Ok(())
        }
    }
}

/// Decodes Image bytes directly into a publisher loan.
pub fn decode_image_into(data: &[u8], header: &mut ImageHeader, payload: &mut [u8]) -> Result<()> {
    decode_image_header_into(data, header)?;
    decode_image_payload_into(data, payload)?;
    Ok(())
}

/// Decodes only the fixed Image header into its final destination.
pub fn decode_image_header_into(data: &[u8], header: &mut ImageHeader) -> Result<()> {
    validate_image(data)?;
    decode_header(data, header)
}

/// Decodes only Image elements into an already-loaned SHM payload.
pub fn decode_image_payload_into(data: &[u8], payload: &mut [u8]) -> Result<()> {
    let count = Image::encoded_element_count(data)?;
    if payload.len() != count {
        return Err(rbt_types::CanonicalDecodeError::DestinationLength {
            expected: count,
            actual: payload.len(),
        }
        .into());
    }
    let offset = ImageHeader::ENCODED_SIZE + std::mem::size_of::<u64>();
    payload.copy_from_slice(&data[offset..]);
    Ok(())
}

/// Decodes PointCloud bytes directly into a publisher loan.
pub fn decode_pointcloud_into(
    data: &[u8],
    header: &mut PointCloudHeader,
    payload: &mut [PointXYZI],
) -> Result<()> {
    decode_pointcloud_header_into(data, header)?;
    decode_pointcloud_payload_into(data, payload)?;
    Ok(())
}

/// Decodes only the fixed PointCloud header into its final destination.
pub fn decode_pointcloud_header_into(data: &[u8], header: &mut PointCloudHeader) -> Result<()> {
    validate_pointcloud(data)?;
    decode_header(data, header)
}

/// Decodes PointCloud elements directly into an already-loaned SHM payload.
pub fn decode_pointcloud_payload_into(data: &[u8], payload: &mut [PointXYZI]) -> Result<()> {
    let count = PointCloud::encoded_element_count(data)?;
    if payload.len() != count {
        return Err(rbt_types::CanonicalDecodeError::DestinationLength {
            expected: count,
            actual: payload.len(),
        }
        .into());
    }
    let offset = PointCloudHeader::ENCODED_SIZE + std::mem::size_of::<u64>();
    let mut cursor = rbt_types::CanonicalCursor::new(&data[offset..]);
    for point in payload {
        point.decode_canonical(&mut cursor)?;
    }
    cursor.finish(data.len() - offset)?;
    Ok(())
}

/// Decodes ControlCommand bytes directly into a publisher loan.
pub fn decode_control_into(data: &[u8], command: &mut ControlCommand) -> Result<()> {
    ensure_message_bound(data)?;
    command.decode_file_into(data)?;
    Ok(())
}

fn validate_image(data: &[u8]) -> Result<()> {
    let count = Image::encoded_element_count(data)?;
    if count > MAX_IMAGE_PAYLOAD_BYTES {
        return Err(Error::PayloadTooLarge {
            channel: KnownMessageType::Image.schema_name(),
            size: count,
            maximum: MAX_IMAGE_PAYLOAD_BYTES,
        });
    }
    let mut header = ImageHeader::default();
    decode_header(data, &mut header)?;
    let channels = match header.encoding {
        PixelFormat::RGB8 | PixelFormat::BGR8 => 3_usize,
        PixelFormat::GRAY8 => 1_usize,
        other => {
            return Err(Error::CorruptRecord {
                channel: KnownMessageType::Image.schema_name(),
                reason: format!("unknown PixelFormat {}", other.value()),
            });
        }
    };
    let width = header.width as usize;
    let height = header.height as usize;
    let stride = header.stride as usize;
    let row_bytes = width
        .checked_mul(channels)
        .ok_or_else(|| Error::CorruptRecord {
            channel: KnownMessageType::Image.schema_name(),
            reason: "Image row byte count overflows".to_owned(),
        })?;
    if width != 0 && height != 0 && stride < row_bytes {
        return Err(Error::CorruptRecord {
            channel: KnownMessageType::Image.schema_name(),
            reason: format!("Image stride {stride} is smaller than row bytes {row_bytes}"),
        });
    }
    let expected = stride
        .checked_mul(height)
        .ok_or_else(|| Error::CorruptRecord {
            channel: KnownMessageType::Image.schema_name(),
            reason: "Image payload size overflows".to_owned(),
        })?;
    if expected != count {
        return Err(Error::CorruptRecord {
            channel: KnownMessageType::Image.schema_name(),
            reason: format!("Image payload length {count} does not equal stride*height {expected}"),
        });
    }
    Ok(())
}

fn validate_pointcloud(data: &[u8]) -> Result<()> {
    let count = PointCloud::encoded_element_count(data)?;
    if count > MAX_POINTCLOUD_POINTS {
        return Err(Error::PayloadTooLarge {
            channel: KnownMessageType::PointCloud.schema_name(),
            size: count,
            maximum: MAX_POINTCLOUD_POINTS,
        });
    }
    let mut header = PointCloudHeader::default();
    decode_header(data, &mut header)?;
    if header.dense > 1 {
        return Err(Error::CorruptRecord {
            channel: KnownMessageType::PointCloud.schema_name(),
            reason: format!("PointCloud dense must be 0 or 1, got {}", header.dense),
        });
    }
    let dimensions = (header.width as usize)
        .checked_mul(header.height as usize)
        .ok_or_else(|| Error::CorruptRecord {
            channel: KnownMessageType::PointCloud.schema_name(),
            reason: "PointCloud width*height overflows".to_owned(),
        })?;
    if dimensions != count {
        return Err(Error::CorruptRecord {
            channel: KnownMessageType::PointCloud.schema_name(),
            reason: format!(
                "PointCloud width*height {dimensions} does not equal element count {count}"
            ),
        });
    }
    Ok(())
}

fn decode_header<H: CanonicalFieldCodec>(data: &[u8], header: &mut H) -> Result<()> {
    let bytes = data
        .get(..H::ENCODED_SIZE)
        .ok_or_else(|| Error::CorruptRecord {
            channel: "rbt canonical header".to_owned(),
            reason: format!(
                "payload has {} bytes but header needs {}",
                data.len(),
                H::ENCODED_SIZE
            ),
        })?;
    let mut cursor = rbt_types::CanonicalCursor::new(bytes);
    header.decode_canonical(&mut cursor)?;
    cursor.finish(bytes.len())?;
    Ok(())
}

fn ensure_message_bound(data: &[u8]) -> Result<()> {
    if data.len() > MAX_MESSAGE_BYTES {
        Err(Error::PayloadTooLarge {
            channel: "MCAP message".to_owned(),
            size: data.len(),
            maximum: MAX_MESSAGE_BYTES,
        })
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn control_golden_bytes_and_roundtrip() {
        let value = ControlCommand {
            sequence: 0x0102_0304_0506_0708,
            value: f32::from_bits(0x7fc0_1234),
            reserved: 0xaabb_ccdd,
        };
        let encoded = encode_control(&value);
        assert_eq!(
            encoded,
            [
                8, 7, 6, 5, 4, 3, 2, 1, 0x34, 0x12, 0xc0, 0x7f, 0xdd, 0xcc, 0xbb, 0xaa,
            ]
        );
        let mut decoded = ControlCommand::default();
        decode_control_into(&encoded, &mut decoded).unwrap();
        assert_eq!(decoded.sequence, value.sequence);
        assert_eq!(decoded.value.to_bits(), value.value.to_bits());
        assert_eq!(decoded.reserved, value.reserved);
    }

    #[test]
    fn image_zero_and_padded_roundtrip() {
        let zero = ImageHeader {
            height: 1,
            ..Default::default()
        };
        let encoded = encode_image(&zero, &[]);
        assert_eq!(
            payload_element_count(KnownMessageType::Image, &encoded).unwrap(),
            0
        );

        let header = ImageHeader {
            sequence: 9,
            timestamp_ns: 17,
            width: 2,
            height: 2,
            stride: 8,
            encoding: PixelFormat::RGB8,
        };
        let payload = (0_u8..16).collect::<Vec<_>>();
        let encoded = encode_image(&header, &payload);
        assert_eq!(encoded.len(), 32 + 8 + payload.len());
        let mut decoded_header = ImageHeader::default();
        let mut decoded_payload = vec![0; payload.len()];
        decode_image_into(&encoded, &mut decoded_header, &mut decoded_payload).unwrap();
        assert_eq!(decoded_header.sequence, 9);
        assert_eq!(decoded_header.stride, 8);
        assert_eq!(decoded_payload, payload);
    }

    #[test]
    fn pointcloud_zero_one_and_float_bits_roundtrip() {
        let empty_header = PointCloudHeader {
            height: 1,
            ..Default::default()
        };
        validate_payload(
            KnownMessageType::PointCloud,
            &encode_pointcloud(&empty_header, &[]),
        )
        .unwrap();

        let header = PointCloudHeader {
            sequence: 5,
            timestamp_ns: 6,
            width: 1,
            height: 1,
            dense: 1,
            reserved: [1, 2, 3, 4, 5, 6, 7],
        };
        let points = [PointXYZI {
            x: f32::from_bits(0x7fc0_0042),
            y: -2.5,
            z: 3.25,
            intensity: 4.5,
        }];
        let encoded = encode_pointcloud(&header, &points);
        let mut decoded_header = PointCloudHeader::default();
        let mut decoded = [PointXYZI::default()];
        decode_pointcloud_into(&encoded, &mut decoded_header, &mut decoded).unwrap();
        assert_eq!(decoded_header.reserved, header.reserved);
        assert_eq!(decoded[0].x.to_bits(), points[0].x.to_bits());
        assert_eq!(decoded[0].y.to_bits(), points[0].y.to_bits());
        assert_eq!(decoded[0].z.to_bits(), points[0].z.to_bits());
        assert_eq!(
            decoded[0].intensity.to_bits(),
            points[0].intensity.to_bits()
        );
    }

    #[test]
    fn malformed_lengths_and_shapes_are_typed() {
        let mut encoded = encode_image(
            &ImageHeader {
                width: 2,
                height: 1,
                stride: 6,
                encoding: PixelFormat::RGB8,
                ..Default::default()
            },
            &[0; 6],
        );
        encoded.pop();
        assert!(validate_payload(KnownMessageType::Image, &encoded).is_err());

        let encoded = encode_pointcloud(
            &PointCloudHeader {
                width: 2,
                height: 1,
                dense: 1,
                ..Default::default()
            },
            &[PointXYZI::default()],
        );
        assert!(validate_payload(KnownMessageType::PointCloud, &encoded).is_err());
    }
}

//! Validated, fixed-size coordinate frame identifiers.

use std::{fmt, str::FromStr};

use serde::{Deserialize, Deserializer, Serialize, Serializer, de::Visitor};

use crate::{TfError, TfResult};

const FRAME_ID_BYTES: usize = 64;

/// A canonical, fixed-size coordinate frame name.
///
/// Names contain one to 63 ASCII bytes. Segments are separated by `/`; a
/// segment starts with an ASCII letter, digit, or underscore, and remaining
/// bytes may also contain `.` and `-`. Unused storage is zero padded.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct FrameId([u8; FRAME_ID_BYTES]);

impl FrameId {
    /// Creates a frame identifier from a validated name.
    pub fn new(value: &str) -> TfResult<Self> {
        if !valid_name(value.as_bytes()) {
            return Err(TfError::InvalidFrame);
        }

        let mut bytes = [0; FRAME_ID_BYTES];
        bytes[..value.len()].copy_from_slice(value.as_bytes());
        Ok(Self(bytes))
    }

    /// Creates a frame identifier from its canonical zero-padded encoding.
    pub fn from_bytes(bytes: [u8; FRAME_ID_BYTES]) -> TfResult<Self> {
        let Some(length) = bytes.iter().position(|byte| *byte == 0) else {
            return Err(TfError::InvalidFrame);
        };
        if bytes[length..].iter().any(|byte| *byte != 0) || !valid_name(&bytes[..length]) {
            return Err(TfError::InvalidFrame);
        }
        Ok(Self(bytes))
    }

    /// Returns this frame identifier's canonical 64-byte encoding.
    pub const fn to_bytes(self) -> [u8; FRAME_ID_BYTES] {
        self.0
    }

    /// Returns this frame identifier as its ASCII name.
    pub fn as_str(&self) -> &str {
        let length = self
            .0
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(FRAME_ID_BYTES);
        // Constructors ensure that the name contains ASCII bytes only.
        std::str::from_utf8(&self.0[..length]).unwrap_or_default()
    }
}

impl fmt::Display for FrameId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

impl FromStr for FrameId {
    type Err = TfError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        Self::new(value)
    }
}

impl TryFrom<&str> for FrameId {
    type Error = TfError;

    fn try_from(value: &str) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl Serialize for FrameId {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_str(self.as_str())
    }
}

impl<'de> Deserialize<'de> for FrameId {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct FrameIdVisitor;

        impl Visitor<'_> for FrameIdVisitor {
            type Value = FrameId;

            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("a valid slash-separated ASCII frame name")
            }

            fn visit_str<E>(self, value: &str) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                FrameId::new(value).map_err(E::custom)
            }
        }

        deserializer.deserialize_str(FrameIdVisitor)
    }
}

fn valid_name(bytes: &[u8]) -> bool {
    if bytes.is_empty() || bytes.len() > FRAME_ID_BYTES - 1 {
        return false;
    }

    let mut segment_start = 0;
    for (index, byte) in bytes.iter().copied().enumerate() {
        if byte == b'/' {
            if !valid_segment(&bytes[segment_start..index]) {
                return false;
            }
            segment_start = index + 1;
        } else if !byte.is_ascii() {
            return false;
        }
    }

    valid_segment(&bytes[segment_start..])
}

fn valid_segment(segment: &[u8]) -> bool {
    let Some((&first, rest)) = segment.split_first() else {
        return false;
    };
    if !is_alphanumeric(first) && first != b'_' {
        return false;
    }
    if segment == b"." || segment == b".." {
        return false;
    }
    rest.iter()
        .all(|byte| is_alphanumeric(*byte) || matches!(*byte, b'_' | b'.' | b'-'))
}

fn is_alphanumeric(byte: u8) -> bool {
    byte.is_ascii_alphanumeric()
}

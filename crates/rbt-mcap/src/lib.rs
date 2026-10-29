//! Durable MCAP recording support for rbt messages.
//!
//! This crate is deliberately outside `rbt-core`. It serializes only at the
//! explicit file boundary and never participates in live shared-memory IPC.

#![forbid(unsafe_code)]

mod codec;
mod container;
mod queue;

pub use codec::{
    decode_control_into, decode_image_header_into, decode_image_into, decode_image_payload_into,
    decode_pointcloud_header_into, decode_pointcloud_into, decode_pointcloud_payload_into,
    encode_control, encode_image, encode_pointcloud, payload_element_count, payload_sequence,
    validate_payload,
};
pub use container::{
    CatalogChannel, ChannelDefinition, ChannelProvenance, McapMessage, MessageVisitor,
    RecordingCatalog, RecordingInfo, RecordingMetadata, RecordingWriter, inspect_file,
    scan_recording, visit_messages, visit_messages_with,
};
pub use queue::{ByteQueue, ByteQueueConsumer, QueueError, QueueItem};
pub use rbt_types::{CANONICAL_FILE_ENCODING_VERSION, CanonicalDecodeError};

use rbt_types::{
    CONTROL_COMMAND_DESCRIPTOR, IMAGE_DESCRIPTOR, MessageTypeDescriptor, POINT_CLOUD_DESCRIPTOR,
    StaticMessageTypeDescriptor,
};
use serde::{Deserialize, Serialize};
use std::{fmt, path::PathBuf};
use thiserror::Error;

/// MCAP Schema encoding for canonical `.rbt` text.
pub const SCHEMA_ENCODING: &str = "rbt-idl-v1";
/// MCAP Channel encoding for deterministic durable rbt payloads.
pub const MESSAGE_ENCODING: &str = "rbt-canonical-v1";
/// MCAP Header profile identifying an rbt recording.
pub const MCAP_PROFILE: &str = "rbt/0.3";
/// Stable library identifier used for deterministic fixture generation.
pub const WRITER_LIBRARY: &str = "rbt-mcap/0.3";

/// Maximum accepted embedded schema text.
pub const MAX_SCHEMA_BYTES: usize = 1024 * 1024;
/// Maximum total key/value bytes accepted for one channel metadata map.
pub const MAX_CHANNEL_METADATA_BYTES: usize = 64 * 1024;
/// Maximum accepted canonical message record.
pub const MAX_MESSAGE_BYTES: usize = 256 * 1024 * 1024;
/// Maximum Image payload accepted by the v0.3 player.
pub const MAX_IMAGE_PAYLOAD_BYTES: usize = 128 * 1024 * 1024;
/// Maximum PointCloud element count accepted by the v0.3 player.
pub const MAX_POINTCLOUD_POINTS: usize = 2_000_000;

/// The rbt message types supported by v0.3 record/replay.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum KnownMessageType {
    /// `rbt.sensor/Image@1`.
    Image,
    /// `rbt.sensor/PointCloud@1`.
    PointCloud,
    /// `rbt.control/ControlCommand@1`.
    ControlCommand,
}

impl KnownMessageType {
    /// Returns the generated descriptor that is authoritative for this build.
    pub const fn descriptor(self) -> StaticMessageTypeDescriptor {
        match self {
            Self::Image => IMAGE_DESCRIPTOR,
            Self::PointCloud => POINT_CLOUD_DESCRIPTOR,
            Self::ControlCommand => CONTROL_COMMAND_DESCRIPTOR,
        }
    }

    /// Returns the stable qualified schema name.
    pub fn schema_name(self) -> String {
        let descriptor = self.descriptor();
        format!(
            "{}/{}@{}",
            descriptor.namespace, descriptor.name, descriptor.abi_major
        )
    }

    /// Returns the generated canonical `.rbt` schema text.
    pub const fn schema_data(self) -> &'static str {
        match self {
            Self::Image => rbt_types::IMAGE_FILE_SCHEMA,
            Self::PointCloud => rbt_types::POINT_CLOUD_FILE_SCHEMA,
            Self::ControlCommand => rbt_types::CONTROL_COMMAND_FILE_SCHEMA,
        }
    }

    /// Resolves a descriptor by logical identity, then requires its exact ABI.
    pub fn from_descriptor(actual: &MessageTypeDescriptor) -> Result<Self> {
        let known = match (actual.namespace.as_str(), actual.name.as_str()) {
            ("rbt.sensor", "Image") => Self::Image,
            ("rbt.sensor", "PointCloud") => Self::PointCloud,
            ("rbt.control", "ControlCommand") => Self::ControlCommand,
            _ => {
                return Err(Error::UnsupportedMessageType {
                    namespace: actual.namespace.clone(),
                    name: actual.name.clone(),
                });
            }
        };
        let expected = known.descriptor().to_owned();
        if actual.abi_major != expected.abi_major {
            return Err(Error::AbiMismatch {
                message: expected.qualified_name(),
                expected: expected.abi_major,
                actual: actual.abi_major,
            });
        }
        if actual.layout_hash != expected.layout_hash {
            return Err(Error::LayoutHashMismatch {
                message: expected.qualified_name(),
                expected: expected.layout_hash.to_string(),
                actual: actual.layout_hash.to_string(),
            });
        }
        if actual != &expected {
            return Err(Error::SchemaMismatch {
                channel: actual.qualified_name(),
                reason: "message descriptor fields differ from generated IDL".to_owned(),
            });
        }
        Ok(known)
    }
}

impl fmt::Display for KnownMessageType {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.descriptor().name)
    }
}

/// Typed recording, container, schema, and codec failures.
#[derive(Debug, Error)]
pub enum Error {
    /// A file could not be opened, read, written, flushed, or synchronized.
    #[error("{operation} {path}: {source}")]
    FileIo {
        /// File operation being attempted.
        operation: &'static str,
        /// Affected file.
        path: PathBuf,
        /// Underlying I/O failure.
        #[source]
        source: std::io::Error,
    },
    /// The MCAP container is malformed or an MCAP writer failed.
    #[error("MCAP container error: {0}")]
    Container(#[from] mcap::McapError),
    /// A record did not contain a supported schema encoding.
    #[error("channel {channel:?} uses unsupported schema encoding {encoding:?}")]
    UnsupportedSchema {
        /// Logical channel topic.
        channel: String,
        /// Observed schema encoding.
        encoding: String,
    },
    /// A record did not contain the rbt canonical message encoding.
    #[error("channel {channel:?} uses unsupported message encoding {encoding:?}")]
    UnsupportedEncoding {
        /// Logical channel topic.
        channel: String,
        /// Observed message encoding.
        encoding: String,
    },
    /// A schema, descriptor, or generated canonical schema disagrees.
    #[error("schema mismatch for {channel:?}: {reason}")]
    SchemaMismatch {
        /// Logical channel topic or message identity.
        channel: String,
        /// Exact mismatch.
        reason: String,
    },
    /// The recorded ABI major is unsupported.
    #[error("ABI mismatch for {message}: expected {expected}, got {actual}")]
    AbiMismatch {
        /// Message identity.
        message: String,
        /// Generated ABI major.
        expected: u16,
        /// Recorded ABI major.
        actual: u16,
    },
    /// The recorded layout hash is unsupported.
    #[error("layout hash mismatch for {message}: expected {expected}, got {actual}")]
    LayoutHashMismatch {
        /// Message identity.
        message: String,
        /// Generated layout hash.
        expected: String,
        /// Recorded layout hash.
        actual: String,
    },
    /// The message namespace/name is not implemented by v0.3.
    #[error("unsupported rbt message type {namespace}/{name}")]
    UnsupportedMessageType {
        /// Recorded namespace.
        namespace: String,
        /// Recorded name.
        name: String,
    },
    /// The durable payload encoding version is not implemented by this build.
    #[error("unsupported rbt file encoding version {actual}; expected {expected}")]
    UnsupportedFileEncodingVersion {
        /// Current generated codec version.
        expected: u16,
        /// Version recorded in file metadata.
        actual: String,
    },
    /// A supported rbt channel requires its file-level provenance metadata.
    #[error("rbt recording metadata is missing: {0}")]
    MissingRecordingMetadata(&'static str),
    /// A canonical payload is truncated or internally inconsistent.
    #[error("canonical decode error: {0}")]
    Decode(#[from] CanonicalDecodeError),
    /// A record violates message-specific semantic invariants.
    #[error("corrupt record on {channel:?}: {reason}")]
    CorruptRecord {
        /// Logical channel or schema identity.
        channel: String,
        /// Exact validation failure.
        reason: String,
    },
    /// A file-controlled payload or element count exceeds a safety limit.
    #[error("payload too large on {channel:?}: {size} exceeds maximum {maximum}")]
    PayloadTooLarge {
        /// Logical channel or record kind.
        channel: String,
        /// Observed byte or element count.
        size: usize,
        /// Configured maximum in the same unit.
        maximum: usize,
    },
    /// A duplicate schema/channel ID disagrees with its first definition.
    #[error("conflicting duplicate {kind} id {id}")]
    ConflictingDefinition {
        /// `schema` or `channel`.
        kind: &'static str,
        /// File-local ID.
        id: u16,
    },
    /// JSON output or metadata encoding failed.
    #[error("JSON error: {0}")]
    Json(#[from] serde_json::Error),
}

/// Crate result type.
pub type Result<T> = std::result::Result<T, Error>;

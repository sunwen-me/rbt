use std::{
    collections::{BTreeMap, btree_map::Entry},
    fs::{self, File},
    io::{BufWriter, Read, Write},
    path::{Path, PathBuf},
};

use mcap::{
    WriteOptions, Writer,
    records::{MessageHeader, Metadata, Record},
    sans_io::{LinearReadEvent, LinearReader, LinearReaderOptions},
};
use rbt_types::{LayoutHash, MessageTypeDescriptor, StreamProfile};
use serde::{Deserialize, Serialize};

use crate::{
    Error, KnownMessageType, MAX_CHANNEL_METADATA_BYTES, MAX_MESSAGE_BYTES, MAX_SCHEMA_BYTES,
    MCAP_PROFILE, MESSAGE_ENCODING, Result, SCHEMA_ENCODING, WRITER_LIBRARY, payload_element_count,
    payload_sequence, validate_payload,
};

const META_NAMESPACE: &str = "rbt.message_namespace";
const META_NAME: &str = "rbt.message_name";
const META_ABI: &str = "rbt.abi_major";
const META_LAYOUT: &str = "rbt.layout_hash";
const META_PROFILE: &str = "rbt.stream_profile";
const META_CAPABILITY_TYPE: &str = "rbt.capability_type";
const META_CAPABILITY_NAME: &str = "rbt.capability_name";
const META_CAPABILITY_QUALIFIER: &str = "rbt.capability_qualifier";
const META_SOURCE_COMPONENT: &str = "rbt.source_component";
const META_SOURCE_PROCESS: &str = "rbt.source_process_instance";
const META_SOURCE_ACTIVATION: &str = "rbt.source_activation";
const META_SOURCE_CAPABILITY: &str = "rbt.source_capability_instance";

/// Historical physical identity attached to one recorded source.
#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct ChannelProvenance {
    /// Stable source component ID.
    pub source_component: String,
    /// Source process lifetime UUID.
    pub source_process_instance: String,
    /// Source activation number.
    pub source_activation: String,
    /// Source physical capability UUID.
    pub source_capability_instance: String,
}

/// Stable logical and ABI description used to create an MCAP Channel.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ChannelDefinition {
    /// Human-readable logical topic.
    pub topic: String,
    /// Exact generated rbt message descriptor.
    pub message_type: MessageTypeDescriptor,
    /// Sensor or Control semantics to reproduce on playback.
    pub profile: StreamProfile,
    /// Logical capability role used by Composer resolution.
    pub capability_type: String,
    /// Human-readable logical capability name.
    pub capability_name: String,
    /// Optional logical qualifier; empty means unqualified.
    pub capability_qualifier: String,
    /// Recorded physical source provenance.
    pub provenance: ChannelProvenance,
}

impl ChannelDefinition {
    fn known_type(&self) -> Result<KnownMessageType> {
        KnownMessageType::from_descriptor(&self.message_type)
    }

    fn metadata(&self) -> BTreeMap<String, String> {
        [
            (
                META_NAMESPACE.to_owned(),
                self.message_type.namespace.clone(),
            ),
            (META_NAME.to_owned(), self.message_type.name.clone()),
            (META_ABI.to_owned(), self.message_type.abi_major.to_string()),
            (
                META_LAYOUT.to_owned(),
                self.message_type.layout_hash.to_string(),
            ),
            (
                META_PROFILE.to_owned(),
                match self.profile {
                    StreamProfile::Sensor => "sensor",
                    StreamProfile::Control => "control",
                }
                .to_owned(),
            ),
            (
                META_CAPABILITY_TYPE.to_owned(),
                self.capability_type.clone(),
            ),
            (
                META_CAPABILITY_NAME.to_owned(),
                self.capability_name.clone(),
            ),
            (
                META_CAPABILITY_QUALIFIER.to_owned(),
                self.capability_qualifier.clone(),
            ),
            (
                META_SOURCE_COMPONENT.to_owned(),
                self.provenance.source_component.clone(),
            ),
            (
                META_SOURCE_PROCESS.to_owned(),
                self.provenance.source_process_instance.clone(),
            ),
            (
                META_SOURCE_ACTIVATION.to_owned(),
                self.provenance.source_activation.clone(),
            ),
            (
                META_SOURCE_CAPABILITY.to_owned(),
                self.provenance.source_capability_instance.clone(),
            ),
        ]
        .into_iter()
        .collect()
    }
}

/// Portable file-level recording provenance.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct RecordingMetadata {
    /// rbt SDK version.
    pub rbt_sdk_version: String,
    /// Canonical IDL language version.
    pub idl_language_version: u16,
    /// Message descriptor format version.
    pub message_descriptor_version: u16,
    /// Composer control protocol version.
    pub control_protocol_version: u16,
    /// Host architecture name.
    pub host_architecture: String,
    /// Host endianness; currently always `little`.
    pub endianness: String,
    /// Epoch-compatible start time used as the MCAP log-time origin.
    pub recording_start_time_ns: u64,
}

impl RecordingMetadata {
    fn record(&self) -> Metadata {
        let metadata = [
            ("rbt_sdk_version", self.rbt_sdk_version.clone()),
            (
                "idl_language_version",
                self.idl_language_version.to_string(),
            ),
            (
                "message_descriptor_version",
                self.message_descriptor_version.to_string(),
            ),
            (
                "file_encoding_version",
                crate::CANONICAL_FILE_ENCODING_VERSION.to_string(),
            ),
            (
                "control_protocol_version",
                self.control_protocol_version.to_string(),
            ),
            ("host_architecture", self.host_architecture.clone()),
            ("endianness", self.endianness.clone()),
            (
                "recording_start_time_ns",
                self.recording_start_time_ns.to_string(),
            ),
        ]
        .into_iter()
        .map(|(key, value)| (key.to_owned(), value))
        .collect();
        Metadata {
            name: "rbt.recording".to_owned(),
            metadata,
        }
    }
}

/// Streaming MCAP writer with stable rbt schema/channel setup.
pub struct RecordingWriter {
    path: PathBuf,
    writer: Option<Writer<BufWriter<File>>>,
    channel_ids: Vec<u16>,
}

impl RecordingWriter {
    /// Creates a new indexed, chunked, uncompressed recording.
    pub fn create(
        path: impl AsRef<Path>,
        channels: &[ChannelDefinition],
        metadata: &RecordingMetadata,
    ) -> Result<Self> {
        let path = path.as_ref().to_owned();
        let file = File::create(&path).map_err(|source| file_error("create", &path, source))?;
        let options = WriteOptions::new()
            .compression(None)
            .profile(MCAP_PROFILE)
            .library(WRITER_LIBRARY);
        let mut writer = options.create(BufWriter::new(file))?;
        writer.write_metadata(&metadata.record())?;

        let mut channel_ids = Vec::with_capacity(channels.len());
        for definition in channels {
            let kind = definition.known_type()?;
            let schema_id = writer.add_schema(
                &kind.schema_name(),
                SCHEMA_ENCODING,
                kind.schema_data().as_bytes(),
            )?;
            let channel_id = writer.add_channel(
                schema_id,
                &definition.topic,
                MESSAGE_ENCODING,
                &definition.metadata(),
            )?;
            channel_ids.push(channel_id);
        }
        Ok(Self {
            path,
            writer: Some(writer),
            channel_ids,
        })
    }

    /// Returns the MCAP channel ID corresponding to a definition index.
    pub fn channel_id(&self, definition_index: usize) -> Option<u16> {
        self.channel_ids.get(definition_index).copied()
    }

    /// Writes one already-canonical payload.
    pub fn write_message(
        &mut self,
        channel_id: u16,
        source_sequence: u64,
        log_time: u64,
        data: &[u8],
    ) -> Result<()> {
        if data.len() > MAX_MESSAGE_BYTES {
            return Err(Error::PayloadTooLarge {
                channel: channel_id.to_string(),
                size: data.len(),
                maximum: MAX_MESSAGE_BYTES,
            });
        }
        let writer = self
            .writer
            .as_mut()
            .expect("writer is present until finish");
        writer.write_to_known_channel(
            &MessageHeader {
                channel_id,
                sequence: source_sequence as u32,
                log_time,
                publish_time: log_time,
            },
            data,
        )?;
        Ok(())
    }

    /// Writes final counters, finalizes indexes, flushes, and syncs the file.
    pub fn finish(mut self, summary: BTreeMap<String, String>) -> Result<()> {
        let mut writer = self.writer.take().expect("writer is present until finish");
        writer.write_metadata(&Metadata {
            name: "rbt.recording.summary".to_owned(),
            metadata: summary,
        })?;
        writer.finish()?;
        let mut output = writer.into_inner();
        output
            .flush()
            .map_err(|source| file_error("flush", &self.path, source))?;
        output
            .get_ref()
            .sync_all()
            .map_err(|source| file_error("sync", &self.path, source))?;
        Ok(())
    }
}

/// One schema shown by `rbt mcap info`.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct SchemaInfo {
    /// File-local schema ID.
    pub id: u16,
    /// Stable schema name.
    pub name: String,
    /// Schema encoding string.
    pub encoding: String,
    /// Embedded schema byte count.
    pub data_bytes: usize,
}

/// One channel and its observed message range.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct CatalogChannel {
    /// File-local channel ID.
    pub id: u16,
    /// Human-readable logical topic.
    pub topic: String,
    /// File-local schema ID.
    pub schema_id: u16,
    /// Schema name, when present.
    pub schema_name: Option<String>,
    /// Schema encoding, when present.
    pub schema_encoding: Option<String>,
    /// Channel message encoding.
    pub message_encoding: String,
    /// Raw channel metadata.
    pub metadata: BTreeMap<String, String>,
    /// Supported generated rbt type, or `None` for an inspectable external channel.
    pub known_type: Option<KnownMessageType>,
    /// Why this channel cannot be replayed, when applicable.
    pub unsupported_reason: Option<String>,
    /// Recorded Sensor/Control profile for supported rbt channels.
    pub profile: Option<StreamProfile>,
    /// Logical capability role.
    pub capability_type: Option<String>,
    /// Logical capability name.
    pub capability_name: Option<String>,
    /// Logical qualifier.
    pub capability_qualifier: Option<String>,
    /// Number of messages observed in file order.
    pub message_count: u64,
    /// Canonical payload bytes observed.
    pub payload_bytes: u64,
    /// First MCAP log time.
    pub first_log_time: Option<u64>,
    /// Last MCAP log time.
    pub last_log_time: Option<u64>,
    /// Largest dynamic element count observed.
    pub maximum_elements: usize,
}

impl CatalogChannel {
    /// Builds the exact descriptor required for a fresh playback publisher.
    pub fn playback_descriptor(&self) -> Option<MessageTypeDescriptor> {
        self.known_type.map(|kind| kind.descriptor().to_owned())
    }
}

/// Validated preflight catalog for playback or inspection.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct RecordingCatalog {
    /// MCAP Header profile.
    pub profile: String,
    /// MCAP Header library identifier.
    pub library: String,
    /// Unique schemas in file-ID order.
    pub schemas: Vec<SchemaInfo>,
    /// Unique channels in file-ID order.
    pub channels: Vec<CatalogChannel>,
    /// Named metadata records in file order.
    pub metadata: Vec<(String, BTreeMap<String, String>)>,
    /// Total data-section messages.
    pub message_count: u64,
    /// Earliest observed log time.
    pub first_log_time: Option<u64>,
    /// Latest observed log time.
    pub last_log_time: Option<u64>,
}

/// Concise file information returned by inspection.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct RecordingInfo {
    /// Inspected path.
    pub path: PathBuf,
    /// File size on disk.
    pub size_bytes: u64,
    /// Total message count.
    pub message_count: u64,
    /// Earliest observed log time.
    pub first_log_time: Option<u64>,
    /// Latest observed log time.
    pub last_log_time: Option<u64>,
    /// Saturating last-first duration.
    pub duration_ns: u64,
    /// MCAP profile.
    pub profile: String,
    /// Writer library.
    pub library: String,
    /// Schemas.
    pub schemas: Vec<SchemaInfo>,
    /// Channels and counts.
    pub channels: Vec<CatalogChannel>,
    /// Named metadata records.
    pub metadata: Vec<(String, BTreeMap<String, String>)>,
}

/// Scans and validates a recording without requiring Composer.
///
/// When `skip_unsupported` is true, non-rbt MCAP channels remain visible in
/// the catalog but are not eligible for playback. An invalid rbt channel is
/// always an error.
pub fn scan_recording(path: impl AsRef<Path>, skip_unsupported: bool) -> Result<RecordingCatalog> {
    let path = path.as_ref();
    let mut state = ScanState::default();
    for_each_record(path, |record| state.consume(record, skip_unsupported))?;
    state.finish()
}

/// Reads concise MCAP information with no runtime/control-plane dependency.
pub fn inspect_file(path: impl AsRef<Path>) -> Result<RecordingInfo> {
    let path = path.as_ref();
    let size_bytes = fs::metadata(path)
        .map_err(|source| file_error("stat", path, source))?
        .len();
    let catalog = scan_recording(path, true)?;
    Ok(RecordingInfo {
        path: path.to_owned(),
        size_bytes,
        message_count: catalog.message_count,
        first_log_time: catalog.first_log_time,
        last_log_time: catalog.last_log_time,
        duration_ns: catalog
            .last_log_time
            .zip(catalog.first_log_time)
            .map_or(0, |(last, first)| last.saturating_sub(first)),
        profile: catalog.profile,
        library: catalog.library,
        schemas: catalog.schemas,
        channels: catalog.channels,
        metadata: catalog.metadata,
    })
}

/// Borrowed message data supplied during a streaming playback pass.
#[derive(Clone, Copy, Debug)]
pub struct McapMessage<'a> {
    /// MCAP sequence hint.
    pub sequence: u32,
    /// Recording timestamp used for scheduling.
    pub log_time: u64,
    /// Conservative publish time; rbt writers set this equal to log time.
    pub publish_time: u64,
    /// Canonical payload bytes, valid only for this callback.
    pub data: &'a [u8],
}

/// Callback used by the safe streaming reader.
pub trait MessageVisitor {
    /// Consumes one validated message in deterministic file order.
    fn visit(&mut self, channel: &CatalogChannel, message: McapMessage<'_>) -> Result<()>;
}

impl<F> MessageVisitor for F
where
    F: FnMut(&CatalogChannel, McapMessage<'_>) -> Result<()>,
{
    fn visit(&mut self, channel: &CatalogChannel, message: McapMessage<'_>) -> Result<()> {
        self(channel, message)
    }
}

/// Streams validated supported messages in file order with bounded memory.
pub fn visit_messages(
    path: impl AsRef<Path>,
    skip_unsupported: bool,
    visitor: &mut impl MessageVisitor,
) -> Result<()> {
    visit_messages_with(path, skip_unsupported, |channel, message| {
        visitor.visit(channel, message)
    })
}

/// Generic-error form of [`visit_messages`] for native playback sinks.
///
/// Container/schema failures are converted through `From<crate::Error>`, while
/// publisher, scheduling, and cancellation failures retain the caller's type.
pub fn visit_messages_with<E, F>(
    path: impl AsRef<Path>,
    skip_unsupported: bool,
    mut visitor: F,
) -> std::result::Result<(), E>
where
    E: From<Error>,
    F: FnMut(&CatalogChannel, McapMessage<'_>) -> std::result::Result<(), E>,
{
    // Complete preflight first so no publisher needs to be created before all
    // channel/schema checks have succeeded.
    let catalog = scan_recording(path.as_ref(), skip_unsupported).map_err(E::from)?;
    let channels = catalog
        .channels
        .into_iter()
        .map(|channel| (channel.id, channel))
        .collect::<BTreeMap<_, _>>();
    for_each_record(path.as_ref(), |record| {
        let Record::Message { header, data } = record else {
            return Ok::<(), E>(());
        };
        let channel = channels.get(&header.channel_id).ok_or_else(|| {
            E::from(Error::SchemaMismatch {
                channel: header.channel_id.to_string(),
                reason: "message references a channel absent from the preflight catalog".to_owned(),
            })
        })?;
        let Some(kind) = channel.known_type else {
            if skip_unsupported {
                return Ok(());
            }
            return Err(E::from(Error::UnsupportedSchema {
                channel: channel.topic.clone(),
                encoding: channel
                    .schema_encoding
                    .clone()
                    .unwrap_or_else(|| "<none>".to_owned()),
            }));
        };
        validate_payload(kind, &data).map_err(E::from)?;
        visitor(
            channel,
            McapMessage {
                sequence: header.sequence,
                log_time: header.log_time,
                publish_time: header.publish_time,
                data: &data,
            },
        )
    })
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct SchemaRecord {
    id: u16,
    name: String,
    encoding: String,
    data: Vec<u8>,
}

#[derive(Default)]
struct ScanState {
    header_seen: bool,
    profile: String,
    library: String,
    schemas: BTreeMap<u16, SchemaRecord>,
    channels: BTreeMap<u16, CatalogChannel>,
    metadata: Vec<(String, BTreeMap<String, String>)>,
    message_count: u64,
    first_log_time: Option<u64>,
    last_log_time: Option<u64>,
}

impl ScanState {
    fn consume(&mut self, record: Record<'_>, skip_unsupported: bool) -> Result<()> {
        match record {
            Record::Header(header) => {
                if !self.header_seen {
                    self.header_seen = true;
                    self.profile = header.profile;
                    self.library = header.library;
                }
            }
            Record::Schema { header, data } => {
                if data.len() > MAX_SCHEMA_BYTES {
                    return Err(Error::PayloadTooLarge {
                        channel: header.name,
                        size: data.len(),
                        maximum: MAX_SCHEMA_BYTES,
                    });
                }
                let schema = SchemaRecord {
                    id: header.id,
                    name: header.name,
                    encoding: header.encoding,
                    data: data.into_owned(),
                };
                match self.schemas.entry(schema.id) {
                    Entry::Vacant(entry) => {
                        entry.insert(schema);
                    }
                    Entry::Occupied(entry) if entry.get() != &schema => {
                        return Err(Error::ConflictingDefinition {
                            kind: "schema",
                            id: schema.id,
                        });
                    }
                    Entry::Occupied(_) => {}
                }
            }
            Record::Channel(channel) => {
                check_metadata_size(&channel.topic, &channel.metadata)?;
                let catalog = catalog_channel(channel, &self.schemas, skip_unsupported)?;
                match self.channels.entry(catalog.id) {
                    Entry::Vacant(entry) => {
                        entry.insert(catalog);
                    }
                    Entry::Occupied(entry) if !same_channel(entry.get(), &catalog) => {
                        return Err(Error::ConflictingDefinition {
                            kind: "channel",
                            id: catalog.id,
                        });
                    }
                    Entry::Occupied(_) => {}
                }
            }
            Record::Message { header, data } => {
                let channel =
                    self.channels
                        .get_mut(&header.channel_id)
                        .ok_or(Error::SchemaMismatch {
                            channel: header.channel_id.to_string(),
                            reason: "message references unknown channel".to_owned(),
                        })?;
                if let Some(kind) = channel.known_type {
                    validate_payload(kind, &data)?;
                    let source_sequence = payload_sequence(kind, &data)?;
                    // The MCAP sequence field is a lossy u32 hint. Its low bits
                    // must still agree with the authoritative payload sequence.
                    if header.sequence != source_sequence as u32 {
                        return Err(Error::CorruptRecord {
                            channel: channel.topic.clone(),
                            reason: format!(
                                "MCAP sequence {} disagrees with payload sequence {}",
                                header.sequence, source_sequence
                            ),
                        });
                    }
                    channel.maximum_elements = channel
                        .maximum_elements
                        .max(payload_element_count(kind, &data)?);
                } else if !skip_unsupported {
                    return Err(Error::UnsupportedSchema {
                        channel: channel.topic.clone(),
                        encoding: channel
                            .schema_encoding
                            .clone()
                            .unwrap_or_else(|| "<none>".to_owned()),
                    });
                }
                channel.message_count = channel.message_count.saturating_add(1);
                channel.payload_bytes = channel
                    .payload_bytes
                    .saturating_add(data.len().try_into().unwrap_or(u64::MAX));
                channel.first_log_time.get_or_insert(header.log_time);
                channel.last_log_time = Some(header.log_time);
                self.message_count = self.message_count.saturating_add(1);
                self.first_log_time = Some(
                    self.first_log_time
                        .map_or(header.log_time, |time| time.min(header.log_time)),
                );
                self.last_log_time = Some(
                    self.last_log_time
                        .map_or(header.log_time, |time| time.max(header.log_time)),
                );
            }
            Record::Metadata(metadata) => {
                self.metadata.push((metadata.name, metadata.metadata));
            }
            _ => {}
        }
        Ok(())
    }

    fn finish(self) -> Result<RecordingCatalog> {
        if !self.header_seen {
            return Err(Error::SchemaMismatch {
                channel: "MCAP header".to_owned(),
                reason: "missing Header record".to_owned(),
            });
        }
        if self
            .channels
            .values()
            .any(|channel| channel.known_type.is_some())
        {
            validate_rbt_recording_metadata(&self.profile, &self.metadata)?;
        }
        Ok(RecordingCatalog {
            profile: self.profile,
            library: self.library,
            schemas: self
                .schemas
                .into_values()
                .map(|schema| SchemaInfo {
                    id: schema.id,
                    name: schema.name,
                    encoding: schema.encoding,
                    data_bytes: schema.data.len(),
                })
                .collect(),
            channels: self.channels.into_values().collect(),
            metadata: self.metadata,
            message_count: self.message_count,
            first_log_time: self.first_log_time,
            last_log_time: self.last_log_time,
        })
    }
}

fn validate_rbt_recording_metadata(
    profile: &str,
    metadata_records: &[(String, BTreeMap<String, String>)],
) -> Result<()> {
    if profile != MCAP_PROFILE {
        return Err(Error::SchemaMismatch {
            channel: "MCAP header".to_owned(),
            reason: format!("profile {profile:?} does not equal {MCAP_PROFILE:?}"),
        });
    }
    let metadata = metadata_records
        .iter()
        .find(|(name, _)| name == "rbt.recording")
        .map(|(_, metadata)| metadata)
        .ok_or(Error::MissingRecordingMetadata("rbt.recording"))?;
    let file_version = metadata
        .get("file_encoding_version")
        .ok_or(Error::MissingRecordingMetadata("file_encoding_version"))?;
    if file_version != &crate::CANONICAL_FILE_ENCODING_VERSION.to_string() {
        return Err(Error::UnsupportedFileEncodingVersion {
            expected: crate::CANONICAL_FILE_ENCODING_VERSION,
            actual: file_version.clone(),
        });
    }
    for (key, expected) in [("idl_language_version", "1"), ("endianness", "little")] {
        let actual = metadata
            .get(key)
            .ok_or(Error::MissingRecordingMetadata(key))?;
        if actual != expected {
            return Err(Error::SchemaMismatch {
                channel: "rbt.recording".to_owned(),
                reason: format!("{key} {actual:?} does not equal supported value {expected:?}"),
            });
        }
    }
    let descriptor_version =
        metadata
            .get("message_descriptor_version")
            .ok_or(Error::MissingRecordingMetadata(
                "message_descriptor_version",
            ))?;
    if descriptor_version != &rbt_types::MESSAGE_DESCRIPTOR_VERSION.to_string() {
        return Err(Error::SchemaMismatch {
            channel: "rbt.recording".to_owned(),
            reason: format!(
                "message_descriptor_version {descriptor_version:?} does not equal supported value {}",
                rbt_types::MESSAGE_DESCRIPTOR_VERSION
            ),
        });
    }
    Ok(())
}

fn catalog_channel(
    channel: mcap::records::Channel,
    schemas: &BTreeMap<u16, SchemaRecord>,
    skip_unsupported: bool,
) -> Result<CatalogChannel> {
    let schema = schemas.get(&channel.schema_id);
    let mut result = CatalogChannel {
        id: channel.id,
        topic: channel.topic,
        schema_id: channel.schema_id,
        schema_name: schema.map(|schema| schema.name.clone()),
        schema_encoding: schema.map(|schema| schema.encoding.clone()),
        message_encoding: channel.message_encoding,
        metadata: channel.metadata,
        known_type: None,
        unsupported_reason: None,
        profile: None,
        capability_type: None,
        capability_name: None,
        capability_qualifier: None,
        message_count: 0,
        payload_bytes: 0,
        first_log_time: None,
        last_log_time: None,
        maximum_elements: 0,
    };
    let Some(schema) = schema else {
        return unsupported_or_error(
            &mut result,
            skip_unsupported,
            "channel has no preceding schema".to_owned(),
        );
    };
    if schema.encoding != SCHEMA_ENCODING {
        return unsupported_or_error(
            &mut result,
            skip_unsupported,
            format!("unsupported schema encoding {:?}", schema.encoding),
        );
    }
    if result.message_encoding != MESSAGE_ENCODING {
        return Err(Error::UnsupportedEncoding {
            channel: result.topic,
            encoding: result.message_encoding,
        });
    }

    let namespace = required_metadata(&result, META_NAMESPACE)?;
    let name = required_metadata(&result, META_NAME)?;
    let abi_major = required_metadata(&result, META_ABI)?
        .parse::<u16>()
        .map_err(|_| Error::SchemaMismatch {
            channel: result.topic.clone(),
            reason: format!("invalid {META_ABI}"),
        })?;
    let layout_hash = required_metadata(&result, META_LAYOUT)?
        .parse::<LayoutHash>()
        .map_err(|error| Error::SchemaMismatch {
            channel: result.topic.clone(),
            reason: format!("invalid {META_LAYOUT}: {error}"),
        })?;
    let identity = MessageTypeDescriptor {
        namespace: namespace.to_owned(),
        name: name.to_owned(),
        abi_major,
        // Exact remaining fields come from the only authoritative generated
        // descriptor after name/ABI/hash preflight below.
        shape: rbt_types::MessageShape::Fixed,
        header_size: 0,
        header_align: 0,
        element_size: 0,
        element_align: 0,
        layout_hash,
    };
    let kind = match (identity.namespace.as_str(), identity.name.as_str()) {
        ("rbt.sensor", "Image") => KnownMessageType::Image,
        ("rbt.sensor", "PointCloud") => KnownMessageType::PointCloud,
        ("rbt.control", "ControlCommand") => KnownMessageType::ControlCommand,
        _ => {
            return unsupported_or_error(
                &mut result,
                skip_unsupported,
                format!(
                    "unsupported rbt message type {}/{}",
                    identity.namespace, identity.name
                ),
            );
        }
    };
    let expected = kind.descriptor();
    if abi_major != expected.abi_major {
        return Err(Error::AbiMismatch {
            message: kind.schema_name(),
            expected: expected.abi_major,
            actual: abi_major,
        });
    }
    if layout_hash != expected.layout_hash {
        return Err(Error::LayoutHashMismatch {
            message: kind.schema_name(),
            expected: expected.layout_hash.to_string(),
            actual: layout_hash.to_string(),
        });
    }
    if schema.name != kind.schema_name() {
        return Err(Error::SchemaMismatch {
            channel: result.topic,
            reason: format!(
                "schema name {:?} does not equal {:?}",
                schema.name,
                kind.schema_name()
            ),
        });
    }
    if schema.data != kind.schema_data().as_bytes() {
        return Err(Error::SchemaMismatch {
            channel: result.topic,
            reason: "embedded canonical IDL differs from generated IDL".to_owned(),
        });
    }
    result.profile = Some(match required_metadata(&result, META_PROFILE)? {
        "sensor" => StreamProfile::Sensor,
        "control" => StreamProfile::Control,
        other => {
            return Err(Error::SchemaMismatch {
                channel: result.topic.clone(),
                reason: format!("unknown stream profile {other:?}"),
            });
        }
    });
    result.capability_type = Some(required_metadata(&result, META_CAPABILITY_TYPE)?.to_owned());
    result.capability_name = Some(required_metadata(&result, META_CAPABILITY_NAME)?.to_owned());
    result.capability_qualifier = Some(
        result
            .metadata
            .get(META_CAPABILITY_QUALIFIER)
            .cloned()
            .unwrap_or_default(),
    );
    result.known_type = Some(kind);
    Ok(result)
}

fn unsupported_or_error(
    channel: &mut CatalogChannel,
    skip_unsupported: bool,
    reason: String,
) -> Result<CatalogChannel> {
    if skip_unsupported {
        channel.unsupported_reason = Some(reason);
        Ok(channel.clone())
    } else {
        Err(Error::UnsupportedSchema {
            channel: channel.topic.clone(),
            encoding: channel
                .schema_encoding
                .clone()
                .unwrap_or_else(|| "<none>".to_owned()),
        })
    }
}

fn required_metadata<'a>(channel: &'a CatalogChannel, key: &str) -> Result<&'a str> {
    channel
        .metadata
        .get(key)
        .map(String::as_str)
        .ok_or_else(|| Error::SchemaMismatch {
            channel: channel.topic.clone(),
            reason: format!("missing channel metadata {key:?}"),
        })
}

fn check_metadata_size(topic: &str, metadata: &BTreeMap<String, String>) -> Result<()> {
    let size = metadata.iter().try_fold(0_usize, |size, (key, value)| {
        size.checked_add(key.len())?.checked_add(value.len())
    });
    let Some(size) = size else {
        return Err(Error::PayloadTooLarge {
            channel: topic.to_owned(),
            size: usize::MAX,
            maximum: MAX_CHANNEL_METADATA_BYTES,
        });
    };
    if size > MAX_CHANNEL_METADATA_BYTES {
        return Err(Error::PayloadTooLarge {
            channel: topic.to_owned(),
            size,
            maximum: MAX_CHANNEL_METADATA_BYTES,
        });
    }
    Ok(())
}

fn same_channel(left: &CatalogChannel, right: &CatalogChannel) -> bool {
    let mut left = left.clone();
    let mut right = right.clone();
    left.message_count = 0;
    left.payload_bytes = 0;
    left.first_log_time = None;
    left.last_log_time = None;
    left.maximum_elements = 0;
    right.message_count = 0;
    right.payload_bytes = 0;
    right.first_log_time = None;
    right.last_log_time = None;
    right.maximum_elements = 0;
    left == right
}

fn for_each_record<E>(
    path: &Path,
    mut visitor: impl FnMut(Record<'_>) -> std::result::Result<(), E>,
) -> std::result::Result<(), E>
where
    E: From<Error>,
{
    let mut file = File::open(path).map_err(|source| E::from(file_error("open", path, source)))?;
    let options = LinearReaderOptions::default()
        .with_check_finishes_after_end_magic(true)
        .with_emit_chunks(false)
        .with_validate_chunk_crcs(true)
        .with_validate_data_section_crc(true)
        .with_validate_summary_section_crc(true)
        .with_record_length_limit(
            MAX_MESSAGE_BYTES
                .saturating_add(MAX_SCHEMA_BYTES)
                .saturating_add(MAX_CHANNEL_METADATA_BYTES),
        );
    let mut reader = LinearReader::new_with_options(options);
    while let Some(event) = reader.next_event() {
        match event.map_err(Error::from).map_err(E::from)? {
            LinearReadEvent::ReadRequest(needed) => {
                let read = file
                    .read(reader.insert(needed))
                    .map_err(|source| E::from(file_error("read", path, source)))?;
                reader.notify_read(read);
            }
            LinearReadEvent::Record { opcode, data } => {
                let record = mcap::parse_record(opcode, data)
                    .map_err(Error::from)
                    .map_err(E::from)?;
                visitor(record)?;
            }
        }
    }
    Ok(())
}

fn file_error(operation: &'static str, path: &Path, source: std::io::Error) -> Error {
    Error::FileIo {
        operation,
        path: path.to_owned(),
        source,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{encode_control, encode_image, encode_pointcloud};
    use rbt_types::{
        ControlCommand, ImageHeader, MESSAGE_DESCRIPTOR_VERSION, PixelFormat, PointCloudHeader,
        PointXYZI,
    };
    use tempfile::tempdir;

    fn definition(
        kind: KnownMessageType,
        topic: &str,
        profile: StreamProfile,
    ) -> ChannelDefinition {
        ChannelDefinition {
            topic: topic.to_owned(),
            message_type: kind.descriptor().to_owned(),
            profile,
            capability_type: kind.to_string(),
            capability_name: topic.trim_start_matches('/').to_owned(),
            capability_qualifier: String::new(),
            provenance: ChannelProvenance::default(),
        }
    }

    fn metadata() -> RecordingMetadata {
        RecordingMetadata {
            rbt_sdk_version: "test".to_owned(),
            idl_language_version: 1,
            message_descriptor_version: MESSAGE_DESCRIPTOR_VERSION,
            control_protocol_version: 1,
            host_architecture: "test".to_owned(),
            endianness: "little".to_owned(),
            recording_start_time_ns: 1_000,
        }
    }

    #[test]
    fn indexed_container_roundtrip_preserves_global_order_and_values() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("mixed.mcap");
        let channels = [
            definition(
                KnownMessageType::Image,
                "/camera/front",
                StreamProfile::Sensor,
            ),
            definition(
                KnownMessageType::PointCloud,
                "/lidar/front",
                StreamProfile::Sensor,
            ),
            definition(
                KnownMessageType::ControlCommand,
                "/control/cmd",
                StreamProfile::Control,
            ),
        ];
        let mut writer = RecordingWriter::create(&path, &channels, &metadata()).unwrap();
        let image = encode_image(
            &ImageHeader {
                sequence: 1,
                timestamp_ns: 10,
                width: 2,
                height: 1,
                stride: 6,
                encoding: PixelFormat::RGB8,
            },
            &[1, 2, 3, 4, 5, 6],
        );
        let control = encode_control(&ControlCommand {
            sequence: 2,
            value: 3.5,
            reserved: 4,
        });
        let cloud = encode_pointcloud(
            &PointCloudHeader {
                sequence: 3,
                timestamp_ns: 30,
                width: 1,
                height: 1,
                dense: 1,
                reserved: [0; 7],
            },
            &[PointXYZI {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                intensity: 4.0,
            }],
        );
        writer
            .write_message(writer.channel_id(0).unwrap(), 1, 1_000, &image)
            .unwrap();
        writer
            .write_message(writer.channel_id(2).unwrap(), 2, 1_020, &control)
            .unwrap();
        writer
            .write_message(writer.channel_id(1).unwrap(), 3, 1_050, &cloud)
            .unwrap();
        writer.finish(BTreeMap::new()).unwrap();

        let info = inspect_file(&path).unwrap();
        assert_eq!(info.message_count, 3);
        assert_eq!(info.duration_ns, 50);
        assert_eq!(info.channels.len(), 3);
        assert!(
            info.channels
                .iter()
                .all(|channel| channel.known_type.is_some())
        );

        let mut order = Vec::new();
        visit_messages(
            &path,
            false,
            &mut |channel: &CatalogChannel, message: McapMessage<'_>| {
                order.push((
                    channel.topic.clone(),
                    message.log_time,
                    message.data.to_vec(),
                ));
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(
            order
                .iter()
                .map(|entry| entry.0.as_str())
                .collect::<Vec<_>>(),
            ["/camera/front", "/control/cmd", "/lidar/front"]
        );
        assert_eq!(order[0].2, image);
        assert_eq!(order[1].2, control);
        assert_eq!(order[2].2, cloud);
    }

    #[test]
    fn corrupt_and_truncated_files_return_errors() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("truncated.mcap");
        fs::write(&path, mcap::MAGIC).unwrap();
        assert!(inspect_file(&path).is_err());
    }

    fn write_control_with_metadata(
        path: &Path,
        schema_encoding: &str,
        message_encoding: &str,
        abi: &str,
        layout: &str,
        file_version: &str,
    ) {
        let file = File::create(path).unwrap();
        let mut writer = WriteOptions::new()
            .compression(None)
            .profile(MCAP_PROFILE)
            .library("metadata-test")
            .create(BufWriter::new(file))
            .unwrap();
        let mut file_metadata = metadata().record();
        file_metadata
            .metadata
            .insert("file_encoding_version".to_owned(), file_version.to_owned());
        writer.write_metadata(&file_metadata).unwrap();
        let schema = writer
            .add_schema(
                &KnownMessageType::ControlCommand.schema_name(),
                schema_encoding,
                KnownMessageType::ControlCommand.schema_data().as_bytes(),
            )
            .unwrap();
        let mut channel_metadata = definition(
            KnownMessageType::ControlCommand,
            "/control/test",
            StreamProfile::Control,
        )
        .metadata();
        channel_metadata.insert(META_ABI.to_owned(), abi.to_owned());
        channel_metadata.insert(META_LAYOUT.to_owned(), layout.to_owned());
        let channel = writer
            .add_channel(schema, "/control/test", message_encoding, &channel_metadata)
            .unwrap();
        let payload = encode_control(&ControlCommand {
            sequence: 1,
            value: 2.0,
            reserved: 3,
        });
        writer
            .write_to_known_channel(
                &MessageHeader {
                    channel_id: channel,
                    sequence: 1,
                    log_time: 1_000,
                    publish_time: 1_000,
                },
                &payload,
            )
            .unwrap();
        writer.finish().unwrap();
    }

    #[test]
    fn schema_abi_layout_and_file_versions_fail_before_visit() {
        let temp = tempdir().unwrap();
        let expected_layout = KnownMessageType::ControlCommand
            .descriptor()
            .layout_hash
            .to_string();

        let unsupported_schema = temp.path().join("unsupported-schema.mcap");
        write_control_with_metadata(
            &unsupported_schema,
            "jsonschema",
            MESSAGE_ENCODING,
            "1",
            &expected_layout,
            "1",
        );
        assert!(matches!(
            scan_recording(&unsupported_schema, false),
            Err(Error::UnsupportedSchema { .. })
        ));

        let unsupported_message = temp.path().join("unsupported-message.mcap");
        write_control_with_metadata(
            &unsupported_message,
            SCHEMA_ENCODING,
            "cdr",
            "1",
            &expected_layout,
            "1",
        );
        assert!(matches!(
            scan_recording(&unsupported_message, false),
            Err(Error::UnsupportedEncoding { .. })
        ));

        let abi = temp.path().join("abi.mcap");
        write_control_with_metadata(
            &abi,
            SCHEMA_ENCODING,
            MESSAGE_ENCODING,
            "2",
            &expected_layout,
            "1",
        );
        assert!(matches!(
            scan_recording(&abi, false),
            Err(Error::AbiMismatch { .. })
        ));

        let layout = temp.path().join("layout.mcap");
        write_control_with_metadata(
            &layout,
            SCHEMA_ENCODING,
            MESSAGE_ENCODING,
            "1",
            "00000000000000000000000000000000",
            "1",
        );
        assert!(matches!(
            scan_recording(&layout, false),
            Err(Error::LayoutHashMismatch { .. })
        ));

        let version = temp.path().join("version.mcap");
        write_control_with_metadata(
            &version,
            SCHEMA_ENCODING,
            MESSAGE_ENCODING,
            "1",
            &expected_layout,
            "2",
        );
        assert!(matches!(
            scan_recording(&version, false),
            Err(Error::UnsupportedFileEncodingVersion { .. })
        ));
    }

    #[test]
    fn external_unknown_channel_is_only_skipped_explicitly() {
        let temp = tempdir().unwrap();
        let path = temp.path().join("external.mcap");
        let file = File::create(&path).unwrap();
        let mut writer = WriteOptions::new()
            .compression(None)
            .profile("external")
            .library("external-test")
            .create(BufWriter::new(file))
            .unwrap();
        let schema = writer
            .add_schema("example", "jsonschema", br#"{"type":"object"}"#)
            .unwrap();
        let channel = writer
            .add_channel(schema, "/external", "json", &BTreeMap::new())
            .unwrap();
        writer
            .write_to_known_channel(
                &MessageHeader {
                    channel_id: channel,
                    sequence: 0,
                    log_time: 7,
                    publish_time: 7,
                },
                b"{}",
            )
            .unwrap();
        writer.finish().unwrap();

        assert!(matches!(
            scan_recording(&path, false),
            Err(Error::UnsupportedSchema { .. })
        ));
        let catalog = scan_recording(&path, true).unwrap();
        assert_eq!(catalog.message_count, 1);
        assert_eq!(catalog.channels[0].message_count, 1);
        assert!(catalog.channels[0].known_type.is_none());
        assert!(catalog.channels[0].unsupported_reason.is_some());
    }
}

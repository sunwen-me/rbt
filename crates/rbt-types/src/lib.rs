//! Stable local shared-memory message types and stream descriptors.
//!
//! This crate describes the ABI used by messages placed in a local
//! iceoryx2 shared-memory segment.  That local ABI is deliberately separate
//! from any network wire format: the descriptor types implement serde for the
//! control plane, but their serialized representation is not a replacement
//! for the shared-memory layout.
//!
//! Only little-endian targets are supported.  A type's layout hash includes
//! the endianness and all layout metadata, so a process cannot accidentally
//! treat a different local representation as the same message ABI.

#![forbid(unsafe_op_in_unsafe_fn)]

#[cfg(target_endian = "big")]
compile_error!("rbt-types supports only little-endian targets");

use std::{fmt, mem, str::FromStr};

use serde::{Deserialize, Deserializer, Serialize, Serializer, de::Visitor};
use thiserror::Error;

/// Domain string included in every canonical layout description.
pub const LAYOUT_DOMAIN: &str = "rbt-shm-abi";

/// Version of the canonical message descriptor format.
pub const MESSAGE_DESCRIPTOR_VERSION: u16 = 1;
/// Backwards-compatible name for [`MESSAGE_DESCRIPTOR_VERSION`].
#[deprecated(note = "use MESSAGE_DESCRIPTOR_VERSION instead")]
pub const LAYOUT_VERSION: u16 = MESSAGE_DESCRIPTOR_VERSION;

/// Version of the deterministic durable-message encoding used in rbt MCAP files.
///
/// This version is independent of the IDL language, shared-memory ABI, message
/// descriptor, and Composer protocol versions.
pub const CANONICAL_FILE_ENCODING_VERSION: u16 = 1;

/// A malformed canonical durable-message payload.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
pub enum CanonicalDecodeError {
    /// The payload ended before a field could be decoded.
    #[error(
        "canonical payload ended at byte {offset}: needed {needed} bytes, {available} available"
    )]
    UnexpectedEof {
        /// Byte offset at which the field starts.
        offset: usize,
        /// Bytes required for the field.
        needed: usize,
        /// Bytes remaining in the payload.
        available: usize,
    },
    /// A fixed or slice message did not consume exactly the supplied bytes.
    #[error("canonical payload length mismatch: expected {expected} bytes, got {actual}")]
    InvalidLength {
        /// Length implied by the schema and element count.
        expected: usize,
        /// Supplied byte length.
        actual: usize,
    },
    /// A dynamic element count cannot be represented or sized safely.
    #[error("canonical element count {count} overflows this platform or encoded size")]
    ElementCountOverflow {
        /// Count read from the payload.
        count: u64,
    },
    /// A caller supplied a destination slice with the wrong element count.
    #[error("canonical destination has {actual} elements; payload requires {expected}")]
    DestinationLength {
        /// Count encoded in the payload.
        expected: usize,
        /// Destination element count.
        actual: usize,
    },
}

/// Bounds-checked cursor over one canonical durable-message payload.
///
/// Generated codecs use this type to decode fields directly into their final
/// destination without reading compiler padding or allocating intermediate
/// message values.
#[derive(Clone, Copy, Debug)]
pub struct CanonicalCursor<'a> {
    remaining: &'a [u8],
    offset: usize,
}

impl<'a> CanonicalCursor<'a> {
    /// Starts decoding at byte zero of `bytes`.
    pub const fn new(bytes: &'a [u8]) -> Self {
        Self {
            remaining: bytes,
            offset: 0,
        }
    }

    /// Returns the number of bytes already consumed.
    pub const fn offset(&self) -> usize {
        self.offset
    }

    /// Returns the bytes not yet consumed.
    pub const fn remaining(&self) -> &'a [u8] {
        self.remaining
    }

    /// Takes exactly `N` bytes or returns a typed truncation error.
    pub fn take<const N: usize>(&mut self) -> Result<[u8; N], CanonicalDecodeError> {
        if self.remaining.len() < N {
            return Err(CanonicalDecodeError::UnexpectedEof {
                offset: self.offset,
                needed: N,
                available: self.remaining.len(),
            });
        }
        let (field, remaining) = self.remaining.split_at(N);
        let mut bytes = [0_u8; N];
        bytes.copy_from_slice(field);
        self.remaining = remaining;
        self.offset += N;
        Ok(bytes)
    }

    /// Requires that every payload byte has been consumed.
    pub fn finish(self, total_len: usize) -> Result<(), CanonicalDecodeError> {
        if self.remaining.is_empty() {
            Ok(())
        } else {
            Err(CanonicalDecodeError::InvalidLength {
                expected: self.offset,
                actual: total_len,
            })
        }
    }
}

/// Field-level codec generated from canonical `.rbt` IDL.
///
/// Implementations encode fields in declaration order and never inspect the
/// in-memory bytes of a Rust or C representation.
pub trait CanonicalFieldCodec: Default {
    /// Number of bytes occupied by this field in `rbt-canonical-v1`.
    const ENCODED_SIZE: usize;

    /// Appends this value's canonical little-endian bytes.
    fn encode_canonical(&self, output: &mut Vec<u8>);

    /// Decodes directly into this value.
    fn decode_canonical(
        &mut self,
        cursor: &mut CanonicalCursor<'_>,
    ) -> Result<(), CanonicalDecodeError>;
}

macro_rules! impl_canonical_scalar {
    ($($ty:ty),+ $(,)?) => {
        $(
            impl CanonicalFieldCodec for $ty {
                const ENCODED_SIZE: usize = ::core::mem::size_of::<Self>();

                fn encode_canonical(&self, output: &mut Vec<u8>) {
                    output.extend_from_slice(&self.to_le_bytes());
                }

                fn decode_canonical(
                    &mut self,
                    cursor: &mut CanonicalCursor<'_>,
                ) -> Result<(), CanonicalDecodeError> {
                    *self = Self::from_le_bytes(cursor.take()?);
                    Ok(())
                }
            }
        )+
    };
}

impl_canonical_scalar!(u8, u16, u32, u64, i8, i16, i32, i64, f32, f64);

impl<T, const N: usize> CanonicalFieldCodec for [T; N]
where
    T: CanonicalFieldCodec + Copy,
    [T; N]: Default,
{
    const ENCODED_SIZE: usize = T::ENCODED_SIZE * N;

    fn encode_canonical(&self, output: &mut Vec<u8>) {
        for value in self {
            value.encode_canonical(output);
        }
    }

    fn decode_canonical(
        &mut self,
        cursor: &mut CanonicalCursor<'_>,
    ) -> Result<(), CanonicalDecodeError> {
        for value in self {
            value.decode_canonical(cursor)?;
        }
        Ok(())
    }
}

/// Generated codec contract for a fixed-size rbt message.
pub trait CanonicalFixedMessage: FixedMessageAbi + CanonicalFieldCodec {
    /// Deterministic canonical `.rbt` schema embedded in MCAP.
    const FILE_SCHEMA: &'static str;

    /// Encodes one message without compiler padding.
    fn encode_file(&self) -> Vec<u8> {
        let mut output = Vec::with_capacity(Self::ENCODED_SIZE);
        self.encode_canonical(&mut output);
        output
    }

    /// Decodes one message directly into the supplied destination.
    fn decode_file_into(&mut self, bytes: &[u8]) -> Result<(), CanonicalDecodeError> {
        if bytes.len() != Self::ENCODED_SIZE {
            return Err(CanonicalDecodeError::InvalidLength {
                expected: Self::ENCODED_SIZE,
                actual: bytes.len(),
            });
        }
        let mut cursor = CanonicalCursor::new(bytes);
        self.decode_canonical(&mut cursor)?;
        cursor.finish(bytes.len())
    }
}

/// Generated codec contract for one header plus a dynamic element slice.
pub trait CanonicalSliceMessage: SliceMessageAbi
where
    Self::Header: CanonicalFieldCodec,
    Self::Element: CanonicalFieldCodec,
{
    /// Deterministic canonical `.rbt` schema embedded in MCAP.
    const FILE_SCHEMA: &'static str;

    /// Encodes the header, element count, and elements without ABI padding.
    fn encode_file(header: &Self::Header, elements: &[Self::Element]) -> Vec<u8> {
        let body_size = elements
            .len()
            .checked_mul(Self::Element::ENCODED_SIZE)
            .and_then(|size| {
                Self::Header::ENCODED_SIZE
                    .checked_add(::core::mem::size_of::<u64>())
                    .and_then(|header| header.checked_add(size))
            })
            .unwrap_or(0);
        let mut output = Vec::with_capacity(body_size);
        header.encode_canonical(&mut output);
        output.extend_from_slice(&(elements.len() as u64).to_le_bytes());
        for element in elements {
            element.encode_canonical(&mut output);
        }
        output
    }

    /// Reads and validates the dynamic element count without allocating.
    fn encoded_element_count(bytes: &[u8]) -> Result<usize, CanonicalDecodeError> {
        let count_offset = Self::Header::ENCODED_SIZE;
        let count_end = count_offset
            .checked_add(::core::mem::size_of::<u64>())
            .ok_or(CanonicalDecodeError::ElementCountOverflow { count: u64::MAX })?;
        let count_bytes =
            bytes
                .get(count_offset..count_end)
                .ok_or(CanonicalDecodeError::UnexpectedEof {
                    offset: count_offset,
                    needed: ::core::mem::size_of::<u64>(),
                    available: bytes.len().saturating_sub(count_offset),
                })?;
        let mut raw = [0_u8; 8];
        raw.copy_from_slice(count_bytes);
        let count = u64::from_le_bytes(raw);
        let count_usize = usize::try_from(count)
            .map_err(|_| CanonicalDecodeError::ElementCountOverflow { count })?;
        let expected = count_usize
            .checked_mul(Self::Element::ENCODED_SIZE)
            .and_then(|size| count_end.checked_add(size))
            .ok_or(CanonicalDecodeError::ElementCountOverflow { count })?;
        if bytes.len() != expected {
            return Err(CanonicalDecodeError::InvalidLength {
                expected,
                actual: bytes.len(),
            });
        }
        Ok(count_usize)
    }

    /// Decodes directly into a final header and element slice.
    fn decode_file_into(
        bytes: &[u8],
        header: &mut Self::Header,
        elements: &mut [Self::Element],
    ) -> Result<(), CanonicalDecodeError> {
        let expected = Self::encoded_element_count(bytes)?;
        if elements.len() != expected {
            return Err(CanonicalDecodeError::DestinationLength {
                expected,
                actual: elements.len(),
            });
        }
        let mut cursor = CanonicalCursor::new(bytes);
        header.decode_canonical(&mut cursor)?;
        let encoded_count = u64::from_le_bytes(cursor.take()?);
        debug_assert_eq!(encoded_count as usize, expected);
        for element in elements {
            element.decode_canonical(&mut cursor)?;
        }
        cursor.finish(bytes.len())
    }
}

/// A data-plane stream's intended profile.
///
/// The profile is control-plane metadata.  It does not alter the underlying
/// memory layout and must not be interpreted as a network serialization mode.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum StreamProfile {
    /// A sensor/data stream such as an image or point cloud.
    Sensor,
    /// A control stream carrying commands or status messages.
    Control,
}

/// Shared-memory resource ceilings advertised by a stream descriptor.
///
/// Resource sizing is transport metadata.  It is deliberately separate from
/// [`StreamProfile`] and from [`MessageTypeDescriptor`], so changing these
/// values never changes a message layout hash.  Publishers must use the
/// advertised values when opening a service; subscribers may request a
/// smaller local borrow ceiling.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct StreamResourceConfig {
    /// Maximum concurrent loans held by one publisher.
    #[serde(default = "default_max_loans")]
    pub max_loans: usize,
    /// Maximum concurrent borrows advertised for one subscriber.
    #[serde(default = "default_max_borrowed")]
    pub max_borrowed: usize,
}

const fn default_max_loans() -> usize {
    2
}

const fn default_max_borrowed() -> usize {
    1
}

impl Default for StreamResourceConfig {
    fn default() -> Self {
        Self {
            max_loans: default_max_loans(),
            max_borrowed: default_max_borrowed(),
        }
    }
}

impl StreamResourceConfig {
    /// Default publisher loan ceiling.
    pub const DEFAULT_MAX_LOANS: usize = 2;
    /// Default subscriber borrow ceiling.
    pub const DEFAULT_MAX_BORROWED: usize = 1;

    /// Creates an explicit resource configuration.
    pub const fn new(max_loans: usize, max_borrowed: usize) -> Self {
        Self {
            max_loans,
            max_borrowed,
        }
    }

    /// Returns this configuration with a different publisher loan ceiling.
    pub const fn with_max_loans(mut self, max_loans: usize) -> Self {
        self.max_loans = max_loans;
        self
    }

    /// Returns this configuration with a different subscriber borrow ceiling.
    pub const fn with_max_borrowed(mut self, max_borrowed: usize) -> Self {
        self.max_borrowed = max_borrowed;
        self
    }
}

/// Readiness protocol advertised by a stream provider.
///
/// Readiness is a transport/deployment capability. It is deliberately kept
/// outside [`MessageTypeDescriptor`] and therefore never contributes to a
/// payload layout hash.
#[derive(
    Clone, Copy, Debug, Default, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize,
)]
#[serde(rename_all = "kebab-case")]
pub enum ReadinessProtocol {
    /// No event-driven readiness protocol is advertised. This is also the
    /// compatibility value for descriptors produced before this field existed.
    #[default]
    None,
    /// The provider publishes advisory readiness events on `<service>/ready`.
    SidecarV1,
}

impl ReadinessProtocol {
    /// Stable human-readable spelling used by diagnostics and the CLI.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::SidecarV1 => "sidecar-v1",
        }
    }
}

impl fmt::Display for ReadinessProtocol {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

/// Transport capabilities advertised with a stream descriptor.
#[derive(
    Clone, Copy, Debug, Default, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize,
)]
pub struct TransportFeatures {
    /// Payload-free readiness mechanism supplied by the provider.
    #[serde(default)]
    pub readiness: ReadinessProtocol,
}

impl TransportFeatures {
    /// Features supplied by the current event-driven rbt transport.
    pub const fn sidecar_v1() -> Self {
        Self {
            readiness: ReadinessProtocol::SidecarV1,
        }
    }
}

/// Whether a message occupies one fixed-size value or a header followed by
/// a run-time-sized element slice.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum MessageShape {
    /// One fixed-size value, represented by the descriptor's header layout;
    /// its element size and alignment are zero.
    Fixed,
    /// One fixed-size header followed by zero or more contiguous elements.
    Slice,
}

impl MessageShape {
    /// Returns the canonical spelling used in layout descriptions.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Fixed => "fixed",
            Self::Slice => "slice",
        }
    }

    /// Returns whether this is a fixed-size message.
    pub const fn is_fixed(self) -> bool {
        matches!(self, Self::Fixed)
    }

    /// Returns whether this is a header-plus-slice message.
    pub const fn is_slice(self) -> bool {
        matches!(self, Self::Slice)
    }
}

/// A 128-bit truncated BLAKE3 hash of a canonical shared-memory layout.
///
/// The wire/debug representation is exactly 32 lowercase hexadecimal
/// characters.  The hash is intentionally short enough to fit conveniently
/// in descriptors while still making accidental ABI collisions impractical
/// for this local runtime.
#[derive(Clone, Copy, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct LayoutHash(pub [u8; 16]);

impl LayoutHash {
    /// Creates a hash from its already-truncated bytes.
    pub const fn new(bytes: [u8; 16]) -> Self {
        Self(bytes)
    }

    /// Hashes a canonical layout description with BLAKE3 and keeps its first
    /// 16 bytes.
    pub fn from_description(description: &str) -> Self {
        let digest = blake3::hash(description.as_bytes());
        let mut bytes = [0_u8; 16];
        bytes.copy_from_slice(&digest.as_bytes()[..16]);
        Self(bytes)
    }

    /// Alias for [`LayoutHash::from_description`].
    pub fn from_canonical_description(description: &str) -> Self {
        Self::from_description(description)
    }

    /// Alias for [`LayoutHash::from_description`].
    pub fn hash(description: &str) -> Self {
        Self::from_description(description)
    }

    /// Returns the raw 16-byte hash.
    pub const fn as_bytes(&self) -> &[u8; 16] {
        &self.0
    }

    /// Returns the raw hash bytes by value.
    pub const fn into_bytes(self) -> [u8; 16] {
        self.0
    }

    /// Parses the canonical lower- or upper-case-free hexadecimal form.
    ///
    /// Uppercase hexadecimal is accepted for interoperability, while all
    /// formatting and serde output remains lowercase.
    pub fn parse(value: &str) -> Result<Self, LayoutHashParseError> {
        value.parse()
    }

    /// Parses the canonical hexadecimal form.
    pub fn from_hex(value: &str) -> Result<Self, LayoutHashParseError> {
        Self::parse(value)
    }
}

impl fmt::Display for LayoutHash {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        for byte in self.0 {
            write!(formatter, "{byte:02x}")?;
        }
        Ok(())
    }
}

impl fmt::Debug for LayoutHash {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, formatter)
    }
}

/// Error returned when a layout hash is not exactly 16-byte hexadecimal.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
pub enum LayoutHashParseError {
    /// The input was not the required 32-byte hexadecimal spelling.
    #[error("layout hash must contain exactly 32 hexadecimal characters, got {length}")]
    InvalidLength {
        /// Number of bytes supplied by the caller.
        length: usize,
    },
    /// A non-hexadecimal byte occurred at the reported position.
    #[error("invalid hexadecimal character {character:?} at byte {index}")]
    InvalidCharacter {
        /// Zero-based byte position of the invalid character.
        index: usize,
        /// Invalid input byte represented as a character when possible.
        character: char,
    },
}

/// Backwards-compatible short name for [`LayoutHashParseError`].
pub type LayoutHashError = LayoutHashParseError;

impl FromStr for LayoutHash {
    type Err = LayoutHashParseError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        if value.len() != 32 {
            return Err(LayoutHashParseError::InvalidLength {
                length: value.len(),
            });
        }

        let mut bytes = [0_u8; 16];
        for (index, pair) in value.as_bytes().as_chunks::<2>().0.iter().enumerate() {
            let high = decode_hex(pair[0]).ok_or(LayoutHashParseError::InvalidCharacter {
                index: index * 2,
                character: char::from(pair[0]),
            })?;
            let low = decode_hex(pair[1]).ok_or(LayoutHashParseError::InvalidCharacter {
                index: index * 2 + 1,
                character: char::from(pair[1]),
            })?;
            bytes[index] = (high << 4) | low;
        }
        Ok(Self(bytes))
    }
}

impl TryFrom<&str> for LayoutHash {
    type Error = LayoutHashParseError;

    fn try_from(value: &str) -> Result<Self, Self::Error> {
        Self::parse(value)
    }
}

impl TryFrom<String> for LayoutHash {
    type Error = LayoutHashParseError;

    fn try_from(value: String) -> Result<Self, Self::Error> {
        Self::parse(&value)
    }
}

impl Serialize for LayoutHash {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_str(&self.to_string())
    }
}

impl<'de> Deserialize<'de> for LayoutHash {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct LayoutHashVisitor;

        impl<'de> Visitor<'de> for LayoutHashVisitor {
            type Value = LayoutHash;

            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("a 32-character hexadecimal layout hash")
            }

            fn visit_str<E>(self, value: &str) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                value.parse().map_err(E::custom)
            }

            fn visit_borrowed_str<E>(self, value: &'de str) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                self.visit_str(value)
            }

            fn visit_string<E>(self, value: String) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                self.visit_str(&value)
            }
        }

        deserializer.deserialize_str(LayoutHashVisitor)
    }
}

fn decode_hex(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

/// Escapes one text component used by the canonical layout description.
///
/// Newlines and backslashes are escaped so that the line-oriented description
/// cannot become ambiguous if a caller supplies unusual metadata.  Ordinary
/// names remain human-readable and appear verbatim.
fn escape_layout_text(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len());
    for character in value.chars() {
        match character {
            '\\' => escaped.push_str("\\\\"),
            '\n' => escaped.push_str("\\n"),
            '\r' => escaped.push_str("\\r"),
            _ => escaped.push(character),
        }
    }
    escaped
}

/// Builds the stable text fed into [`LayoutHash::from_description`].
///
/// The format is intentionally explicit rather than relying on Rust's debug
/// representation.  It contains a domain and format version, little-endian
/// marker, shape, type identity, all header/element size and alignment
/// values, and the canonical field signature.  Size/alignment values are
/// rendered as decimal numbers; the target endianness is fixed by the crate's
/// compile-time guard.  The size/alignment arguments are the descriptor's
/// explicit `u32` wire values.
#[allow(clippy::too_many_arguments)]
pub fn canonical_layout_description(
    namespace: &str,
    name: &str,
    abi_major: u16,
    shape: MessageShape,
    header_size: u32,
    header_align: u32,
    element_size: u32,
    element_align: u32,
    signature: &str,
) -> String {
    format!(
        "domain={LAYOUT_DOMAIN}\nversion={MESSAGE_DESCRIPTOR_VERSION}\nendianness=little\nshape={}\nnamespace={}\nname={}\nabi_major={abi_major}\nheader_size={header_size}\nheader_align={header_align}\nelement_size={element_size}\nelement_align={element_align}\nsignature={}\n",
        shape.as_str(),
        escape_layout_text(namespace),
        escape_layout_text(name),
        escape_layout_text(signature),
    )
}

/// A message ABI description whose type identity is borrowed from static data.
///
/// Generated message definitions can expose one of these descriptors as a
/// compile-time associated constant without allocating owned strings.  Use
/// [`StaticMessageTypeDescriptor::to_owned`] or
/// [`StaticMessageTypeDescriptor::into_owned`] when a wire-friendly owned
/// [`MessageTypeDescriptor`] is needed.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct StaticMessageTypeDescriptor {
    /// Stable type namespace.
    pub namespace: &'static str,
    /// Stable type name within [`Self::namespace`].
    pub name: &'static str,
    /// Major ABI version.  Incompatible layout changes require a new major.
    pub abi_major: u16,
    /// Fixed or header-plus-slice shape.
    pub shape: MessageShape,
    /// Header size in bytes.  Fixed messages use the fixed value's size.
    pub header_size: u32,
    /// Header alignment in bytes.  Fixed messages use the fixed value's
    /// alignment.
    pub header_align: u32,
    /// Element size in bytes.  Fixed messages use zero; slices use the
    /// repeated element type's size.
    pub element_size: u32,
    /// Element alignment in bytes.  Fixed messages use zero; slices use the
    /// repeated element type's alignment.
    pub element_align: u32,
    /// Truncated BLAKE3 hash over the canonical layout description.
    pub layout_hash: LayoutHash,
}

impl StaticMessageTypeDescriptor {
    /// Constructs a static descriptor from all wire fields.
    #[allow(clippy::too_many_arguments)]
    pub const fn new(
        namespace: &'static str,
        name: &'static str,
        abi_major: u16,
        shape: MessageShape,
        header_size: u32,
        header_align: u32,
        element_size: u32,
        element_align: u32,
        layout_hash: LayoutHash,
    ) -> Self {
        Self {
            namespace,
            name,
            abi_major,
            shape,
            header_size,
            header_align,
            element_size,
            element_align,
            layout_hash,
        }
    }

    /// Converts this static descriptor into an owned descriptor.
    pub fn to_owned(&self) -> MessageTypeDescriptor {
        (*self).into_owned()
    }

    /// Converts this static descriptor into an owned descriptor.
    pub fn into_owned(self) -> MessageTypeDescriptor {
        MessageTypeDescriptor::new(
            self.namespace,
            self.name,
            self.abi_major,
            self.shape,
            self.header_size,
            self.header_align,
            self.element_size,
            self.element_align,
            self.layout_hash,
        )
    }
}

impl From<StaticMessageTypeDescriptor> for MessageTypeDescriptor {
    fn from(value: StaticMessageTypeDescriptor) -> Self {
        value.into_owned()
    }
}

impl From<&StaticMessageTypeDescriptor> for MessageTypeDescriptor {
    fn from(value: &StaticMessageTypeDescriptor) -> Self {
        value.to_owned()
    }
}

/// A complete, wire-friendly description of one message ABI.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct MessageTypeDescriptor {
    /// Stable type namespace.
    pub namespace: String,
    /// Stable type name within [`Self::namespace`].
    pub name: String,
    /// Major ABI version.  Incompatible layout changes require a new major.
    pub abi_major: u16,
    /// Fixed or header-plus-slice shape.
    pub shape: MessageShape,
    /// Header size in bytes.  Fixed messages use the fixed value's size.
    pub header_size: u32,
    /// Header alignment in bytes.  Fixed messages use the fixed value's
    /// alignment.
    pub header_align: u32,
    /// Element size in bytes.  Fixed messages use zero; slices use the
    /// repeated element type's size.
    pub element_size: u32,
    /// Element alignment in bytes.  Fixed messages use zero; slices use the
    /// repeated element type's alignment.
    pub element_align: u32,
    /// Truncated BLAKE3 hash over the canonical layout description.
    pub layout_hash: LayoutHash,
}

impl MessageTypeDescriptor {
    /// Constructs a descriptor from all wire fields.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        namespace: impl Into<String>,
        name: impl Into<String>,
        abi_major: u16,
        shape: MessageShape,
        header_size: u32,
        header_align: u32,
        element_size: u32,
        element_align: u32,
        layout_hash: LayoutHash,
    ) -> Self {
        Self {
            namespace: namespace.into(),
            name: name.into(),
            abi_major,
            shape,
            header_size,
            header_align,
            element_size,
            element_align,
            layout_hash,
        }
    }

    /// Returns a descriptor generated from a fixed-message ABI trait.
    pub fn from_fixed<T: FixedMessageAbi>() -> Self {
        T::descriptor()
    }

    /// Alias for [`MessageTypeDescriptor::from_fixed`].
    pub fn for_fixed<T: FixedMessageAbi>() -> Self {
        Self::from_fixed::<T>()
    }

    /// Returns a descriptor generated from a slice-message ABI trait.
    pub fn from_slice<T: SliceMessageAbi>() -> Self {
        T::descriptor()
    }

    /// Alias for [`MessageTypeDescriptor::from_slice`].
    pub fn for_slice<T: SliceMessageAbi>() -> Self {
        Self::from_slice::<T>()
    }

    /// Returns the canonical `namespace/name@major` identity.
    pub fn qualified_name(&self) -> String {
        format!("{}/{}@{}", self.namespace, self.name, self.abi_major)
    }

    /// Alias for [`MessageTypeDescriptor::qualified_name`].
    pub fn type_name(&self) -> String {
        self.qualified_name()
    }

    /// Returns the stable namespace.
    pub fn namespace(&self) -> &str {
        &self.namespace
    }

    /// Returns the stable name.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Returns the ABI major version.
    pub const fn abi_major(&self) -> u16 {
        self.abi_major
    }

    /// Returns this message's shape.
    pub const fn shape(&self) -> MessageShape {
        self.shape
    }

    /// Returns the layout hash.
    pub const fn layout_hash(&self) -> LayoutHash {
        self.layout_hash
    }

    /// Returns whether this descriptor describes a fixed-size value.
    pub const fn is_fixed(&self) -> bool {
        self.shape.is_fixed()
    }

    /// Returns whether this descriptor describes a slice message.
    pub const fn is_slice(&self) -> bool {
        self.shape.is_slice()
    }
}

impl fmt::Display for MessageTypeDescriptor {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.qualified_name())
    }
}

/// A stream endpoint and its complete message/profile metadata.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct StreamDescriptor {
    /// Service name used by the local transport.
    pub service_name: String,
    /// Complete message ABI descriptor.
    pub message_type: MessageTypeDescriptor,
    /// Intended stream profile.
    pub profile: StreamProfile,
    /// Transport/deployment capabilities, separate from the payload ABI.
    /// Missing fields from pre-v0.2 descriptors intentionally mean no
    /// advertised features so a new WaitSet consumer fails explicitly.
    #[serde(default)]
    pub transport: TransportFeatures,
    /// Shared-memory resource ceilings advertised by the provider.
    ///
    /// This metadata is independent of the message ABI and therefore never
    /// contributes to [`MessageTypeDescriptor::layout_hash`].  The serde
    /// default keeps descriptors written before resource metadata compatible.
    #[serde(default)]
    pub resources: StreamResourceConfig,
}

impl StreamDescriptor {
    /// Constructs a descriptor from its wire fields.
    pub fn new(
        service_name: impl Into<String>,
        message_type: MessageTypeDescriptor,
        profile: StreamProfile,
    ) -> Self {
        Self::new_with_transport_and_resources(
            service_name,
            message_type,
            profile,
            TransportFeatures::sidecar_v1(),
            StreamResourceConfig::default(),
        )
    }

    /// Constructs a descriptor with explicit transport capabilities.
    pub fn new_with_transport(
        service_name: impl Into<String>,
        message_type: MessageTypeDescriptor,
        profile: StreamProfile,
        transport: TransportFeatures,
    ) -> Self {
        Self::new_with_transport_and_resources(
            service_name,
            message_type,
            profile,
            transport,
            StreamResourceConfig::default(),
        )
    }

    /// Constructs a descriptor with explicit shared-memory resources and the
    /// default event-driven transport capabilities.
    pub fn new_with_resources(
        service_name: impl Into<String>,
        message_type: MessageTypeDescriptor,
        profile: StreamProfile,
        resources: StreamResourceConfig,
    ) -> Self {
        Self::new_with_transport_and_resources(
            service_name,
            message_type,
            profile,
            TransportFeatures::sidecar_v1(),
            resources,
        )
    }

    /// Constructs a descriptor with explicit transport capabilities and
    /// shared-memory resources.
    pub fn new_with_transport_and_resources(
        service_name: impl Into<String>,
        message_type: MessageTypeDescriptor,
        profile: StreamProfile,
        transport: TransportFeatures,
        resources: StreamResourceConfig,
    ) -> Self {
        Self {
            service_name: service_name.into(),
            message_type,
            profile,
            transport,
            resources,
        }
    }

    /// Constructs a fixed-message stream descriptor.
    pub fn for_fixed<T: FixedMessageAbi>(
        service_name: impl Into<String>,
        profile: StreamProfile,
    ) -> Self {
        Self::new(service_name, T::descriptor(), profile)
    }

    /// Constructs a fixed-message stream descriptor with explicit resources.
    pub fn for_fixed_with_resources<T: FixedMessageAbi>(
        service_name: impl Into<String>,
        profile: StreamProfile,
        resources: StreamResourceConfig,
    ) -> Self {
        Self::new_with_resources(service_name, T::descriptor(), profile, resources)
    }

    /// Constructs a slice-message stream descriptor.
    pub fn for_slice<T: SliceMessageAbi>(
        service_name: impl Into<String>,
        profile: StreamProfile,
    ) -> Self {
        Self::new(service_name, T::descriptor(), profile)
    }

    /// Constructs a slice-message stream descriptor with explicit resources.
    pub fn for_slice_with_resources<T: SliceMessageAbi>(
        service_name: impl Into<String>,
        profile: StreamProfile,
        resources: StreamResourceConfig,
    ) -> Self {
        Self::new_with_resources(service_name, T::descriptor(), profile, resources)
    }

    /// Returns the transport service name.
    pub fn service_name(&self) -> &str {
        &self.service_name
    }

    /// Returns the complete message descriptor.
    pub fn message_type(&self) -> &MessageTypeDescriptor {
        &self.message_type
    }

    /// Alias for [`StreamDescriptor::message_type`].
    pub fn type_descriptor(&self) -> &MessageTypeDescriptor {
        self.message_type()
    }

    /// Returns the stream profile.
    pub const fn profile(&self) -> StreamProfile {
        self.profile
    }

    /// Returns the advertised transport capabilities.
    pub const fn transport(&self) -> TransportFeatures {
        self.transport
    }

    /// Returns the advertised shared-memory resource ceilings.
    pub const fn resources(&self) -> StreamResourceConfig {
        self.resources
    }

    /// Returns the provider's advertised readiness protocol.
    pub const fn readiness(&self) -> ReadinessProtocol {
        self.transport.readiness
    }
}

/// Readiness protocol advertised by an RPC provider.
///
/// RPC readiness has its own protocol namespace.  It is intentionally not
/// [`ReadinessProtocol`]: a data-plane stream's sidecar cannot be mistaken for
/// an RPC sidecar by a consumer that validates the descriptor.
#[derive(
    Clone, Copy, Debug, Default, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize,
)]
#[serde(rename_all = "kebab-case")]
pub enum RpcReadinessProtocol {
    /// No event-driven readiness protocol is advertised.
    #[default]
    None,
    /// The provider publishes RPC readiness events on the RPC sidecar.
    RpcSidecarV1,
}

impl RpcReadinessProtocol {
    /// Stable human-readable spelling used by diagnostics and the CLI.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::RpcSidecarV1 => "rpc-sidecar-v1",
        }
    }
}

impl fmt::Display for RpcReadinessProtocol {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

/// Transport capabilities advertised with an RPC descriptor.
#[derive(
    Clone, Copy, Debug, Default, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize,
)]
pub struct RpcTransportFeatures {
    /// Payload-free readiness mechanism supplied by the RPC provider.
    #[serde(default)]
    pub readiness: RpcReadinessProtocol,
}

impl RpcTransportFeatures {
    /// Features supplied by the current RPC sidecar transport.
    pub const fn rpc_sidecar_v1() -> Self {
        Self {
            readiness: RpcReadinessProtocol::RpcSidecarV1,
        }
    }

    /// Constructs explicit RPC transport capabilities.
    pub const fn new(readiness: RpcReadinessProtocol) -> Self {
        Self { readiness }
    }

    /// Returns the advertised RPC readiness protocol.
    pub const fn readiness(self) -> RpcReadinessProtocol {
        self.readiness
    }
}

const fn default_rpc_max_in_flight() -> usize {
    4
}

const fn default_rpc_max_clients() -> usize {
    16
}

const fn default_rpc_max_request_loans() -> usize {
    2
}

const fn default_rpc_response_buffer_size() -> usize {
    1
}

const fn default_rpc_max_borrowed_responses() -> usize {
    1
}

const fn default_rpc_max_response_loans() -> usize {
    1
}

const fn default_rpc_max_request_slice_len() -> usize {
    1
}

const fn default_rpc_max_response_slice_len() -> usize {
    1
}

/// Shared-memory request/response resource ceilings advertised by an RPC
/// descriptor.
///
/// The fields correspond to the request/response settings in iceoryx2 0.9.3:
/// `max_in_flight` maps to `max_active_requests_per_client`,
/// `max_request_loans` to `max_loaned_requests`, `response_buffer_size` to
/// `max_response_buffer_size`, `max_borrowed_responses` to
/// `max_borrowed_responses_per_pending_response`, and `max_response_loans` to
/// `server_max_loaned_responses_per_request`.  The slice-length ceilings map
/// to the local port `initial_max_slice_len` settings.  Fixed request/response
/// types ignore them; a slice request's client and a slice response's server
/// use their corresponding ceiling.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct RpcResourceConfig {
    /// Maximum active requests held in flight per client.
    #[serde(default = "default_rpc_max_in_flight")]
    pub max_in_flight: usize,
    /// Maximum clients supported by the service.
    #[serde(default = "default_rpc_max_clients")]
    pub max_clients: usize,
    /// Maximum request loans held by one client.
    #[serde(default = "default_rpc_max_request_loans")]
    pub max_request_loans: usize,
    /// Response queue capacity for each client/server connection.
    #[serde(default = "default_rpc_response_buffer_size")]
    pub response_buffer_size: usize,
    /// Maximum responses borrowed per pending response by one client.
    #[serde(default = "default_rpc_max_borrowed_responses")]
    pub max_borrowed_responses: usize,
    /// Maximum responses loaned per request by one server.
    #[serde(default = "default_rpc_max_response_loans")]
    pub max_response_loans: usize,
    /// Initial maximum request slice length for a slice request client.
    #[serde(default = "default_rpc_max_request_slice_len")]
    pub max_request_slice_len: usize,
    /// Initial maximum response slice length for a slice response server.
    #[serde(default = "default_rpc_max_response_slice_len")]
    pub max_response_slice_len: usize,
}

impl Default for RpcResourceConfig {
    fn default() -> Self {
        Self {
            max_in_flight: default_rpc_max_in_flight(),
            max_clients: default_rpc_max_clients(),
            max_request_loans: default_rpc_max_request_loans(),
            response_buffer_size: default_rpc_response_buffer_size(),
            max_borrowed_responses: default_rpc_max_borrowed_responses(),
            max_response_loans: default_rpc_max_response_loans(),
            max_request_slice_len: default_rpc_max_request_slice_len(),
            max_response_slice_len: default_rpc_max_response_slice_len(),
        }
    }
}

impl RpcResourceConfig {
    /// Default maximum active requests held in flight per client.
    pub const DEFAULT_MAX_IN_FLIGHT: usize = 4;
    /// Default maximum clients supported by the service.
    pub const DEFAULT_MAX_CLIENTS: usize = 16;
    /// Default maximum request loans held by one client.
    pub const DEFAULT_MAX_REQUEST_LOANS: usize = 2;
    /// Default response queue capacity for each connection.
    pub const DEFAULT_RESPONSE_BUFFER_SIZE: usize = 1;
    /// Default maximum responses borrowed per pending response.
    pub const DEFAULT_MAX_BORROWED_RESPONSES: usize = 1;
    /// Default maximum responses loaned per request by one server.
    pub const DEFAULT_MAX_RESPONSE_LOANS: usize = 1;
    /// Default initial maximum request slice length.
    pub const DEFAULT_MAX_REQUEST_SLICE_LEN: usize = 1;
    /// Default initial maximum response slice length.
    pub const DEFAULT_MAX_RESPONSE_SLICE_LEN: usize = 1;

    /// Creates an explicit RPC resource configuration.
    // Keep this exhaustive constructor aligned with the eight independent
    // transport limits; splitting it would make explicit resource sizing less
    // auditable at call sites.
    #[allow(clippy::too_many_arguments)]
    pub const fn new(
        max_in_flight: usize,
        max_clients: usize,
        max_request_loans: usize,
        response_buffer_size: usize,
        max_borrowed_responses: usize,
        max_response_loans: usize,
        max_request_slice_len: usize,
        max_response_slice_len: usize,
    ) -> Self {
        Self {
            max_in_flight,
            max_clients,
            max_request_loans,
            response_buffer_size,
            max_borrowed_responses,
            max_response_loans,
            max_request_slice_len,
            max_response_slice_len,
        }
    }

    /// Returns this configuration with a different in-flight request ceiling.
    pub const fn with_max_in_flight(mut self, max_in_flight: usize) -> Self {
        self.max_in_flight = max_in_flight;
        self
    }

    /// Returns this configuration with a different client ceiling.
    pub const fn with_max_clients(mut self, max_clients: usize) -> Self {
        self.max_clients = max_clients;
        self
    }

    /// Returns this configuration with a different request-loan ceiling.
    pub const fn with_max_request_loans(mut self, max_request_loans: usize) -> Self {
        self.max_request_loans = max_request_loans;
        self
    }

    /// Returns this configuration with a different response buffer size.
    pub const fn with_response_buffer_size(mut self, response_buffer_size: usize) -> Self {
        self.response_buffer_size = response_buffer_size;
        self
    }

    /// Returns this configuration with a different borrowed-response ceiling.
    pub const fn with_max_borrowed_responses(mut self, max_borrowed_responses: usize) -> Self {
        self.max_borrowed_responses = max_borrowed_responses;
        self
    }

    /// Returns this configuration with a different response-loan ceiling.
    pub const fn with_max_response_loans(mut self, max_response_loans: usize) -> Self {
        self.max_response_loans = max_response_loans;
        self
    }

    /// Returns this configuration with a different request slice ceiling.
    pub const fn with_max_request_slice_len(mut self, max_request_slice_len: usize) -> Self {
        self.max_request_slice_len = max_request_slice_len;
        self
    }

    /// Returns this configuration with a different response slice ceiling.
    pub const fn with_max_response_slice_len(mut self, max_response_slice_len: usize) -> Self {
        self.max_response_slice_len = max_response_slice_len;
        self
    }

    /// Maximum active requests held in flight per client.
    pub const fn max_in_flight(&self) -> usize {
        self.max_in_flight
    }

    /// Maximum clients supported by the service.
    pub const fn max_clients(&self) -> usize {
        self.max_clients
    }

    /// Maximum request loans held by one client.
    pub const fn max_request_loans(&self) -> usize {
        self.max_request_loans
    }

    /// Response queue capacity for each client/server connection.
    pub const fn response_buffer_size(&self) -> usize {
        self.response_buffer_size
    }

    /// Maximum responses borrowed per pending response by one client.
    pub const fn max_borrowed_responses(&self) -> usize {
        self.max_borrowed_responses
    }

    /// Maximum responses loaned per request by one server.
    pub const fn max_response_loans(&self) -> usize {
        self.max_response_loans
    }

    /// Initial maximum request slice length for a slice request client.
    pub const fn max_request_slice_len(&self) -> usize {
        self.max_request_slice_len
    }

    /// Initial maximum response slice length for a slice response server.
    pub const fn max_response_slice_len(&self) -> usize {
        self.max_response_slice_len
    }
}

/// An RPC endpoint and its complete request/response metadata.
///
/// Request and response descriptors are ordinary [`MessageTypeDescriptor`]
/// values.  Their layout hashes therefore retain exactly the same canonical
/// meaning as descriptors used by data-plane streams.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct RpcDescriptor {
    /// Service name used by the local request/response transport.
    pub service_name: String,
    /// Complete request message ABI descriptor.
    #[serde(alias = "request")]
    pub request_type: MessageTypeDescriptor,
    /// Complete response message ABI descriptor.
    #[serde(alias = "response")]
    pub response_type: MessageTypeDescriptor,
    /// RPC-specific transport/deployment capabilities.
    #[serde(default)]
    pub transport: RpcTransportFeatures,
    /// Shared-memory request/response resource ceilings advertised by the
    /// provider.
    #[serde(default)]
    pub resources: RpcResourceConfig,
}

impl RpcDescriptor {
    /// Constructs a descriptor from its wire fields using the current RPC
    /// sidecar and bounded resource defaults.
    pub fn new(
        service_name: impl Into<String>,
        request_type: MessageTypeDescriptor,
        response_type: MessageTypeDescriptor,
    ) -> Self {
        Self::new_with_transport_and_resources(
            service_name,
            request_type,
            response_type,
            RpcTransportFeatures::rpc_sidecar_v1(),
            RpcResourceConfig::default(),
        )
    }

    /// Constructs a descriptor with explicit RPC transport capabilities.
    pub fn new_with_transport(
        service_name: impl Into<String>,
        request_type: MessageTypeDescriptor,
        response_type: MessageTypeDescriptor,
        transport: RpcTransportFeatures,
    ) -> Self {
        Self::new_with_transport_and_resources(
            service_name,
            request_type,
            response_type,
            transport,
            RpcResourceConfig::default(),
        )
    }

    /// Constructs a descriptor with explicit resource ceilings and the
    /// default current RPC sidecar.
    pub fn new_with_resources(
        service_name: impl Into<String>,
        request_type: MessageTypeDescriptor,
        response_type: MessageTypeDescriptor,
        resources: RpcResourceConfig,
    ) -> Self {
        Self::new_with_transport_and_resources(
            service_name,
            request_type,
            response_type,
            RpcTransportFeatures::rpc_sidecar_v1(),
            resources,
        )
    }

    /// Constructs a descriptor with explicit transport capabilities and
    /// resource ceilings.
    pub fn new_with_transport_and_resources(
        service_name: impl Into<String>,
        request_type: MessageTypeDescriptor,
        response_type: MessageTypeDescriptor,
        transport: RpcTransportFeatures,
        resources: RpcResourceConfig,
    ) -> Self {
        Self {
            service_name: service_name.into(),
            request_type,
            response_type,
            transport,
            resources,
        }
    }

    /// Constructs a fixed-request/fixed-response RPC descriptor.
    pub fn for_fixed<Request, Response>(service_name: impl Into<String>) -> Self
    where
        Request: FixedMessageAbi,
        Response: FixedMessageAbi,
    {
        Self::new(service_name, Request::descriptor(), Response::descriptor())
    }

    /// Constructs a fixed-request/slice-response RPC descriptor.
    pub fn for_fixed_slice<Request, Response>(service_name: impl Into<String>) -> Self
    where
        Request: FixedMessageAbi,
        Response: SliceMessageAbi,
    {
        Self::new(service_name, Request::descriptor(), Response::descriptor())
    }

    /// Constructs a slice-request/fixed-response RPC descriptor.
    pub fn for_slice_fixed<Request, Response>(service_name: impl Into<String>) -> Self
    where
        Request: SliceMessageAbi,
        Response: FixedMessageAbi,
    {
        Self::new(service_name, Request::descriptor(), Response::descriptor())
    }

    /// Constructs a slice-request/slice-response RPC descriptor.
    pub fn for_slice<Request, Response>(service_name: impl Into<String>) -> Self
    where
        Request: SliceMessageAbi,
        Response: SliceMessageAbi,
    {
        Self::new(service_name, Request::descriptor(), Response::descriptor())
    }

    /// Constructs a fixed-request/fixed-response descriptor with resources.
    pub fn for_fixed_with_resources<Request, Response>(
        service_name: impl Into<String>,
        resources: RpcResourceConfig,
    ) -> Self
    where
        Request: FixedMessageAbi,
        Response: FixedMessageAbi,
    {
        Self::new_with_resources(
            service_name,
            Request::descriptor(),
            Response::descriptor(),
            resources,
        )
    }

    /// Constructs a fixed-request/slice-response descriptor with resources.
    pub fn for_fixed_slice_with_resources<Request, Response>(
        service_name: impl Into<String>,
        resources: RpcResourceConfig,
    ) -> Self
    where
        Request: FixedMessageAbi,
        Response: SliceMessageAbi,
    {
        Self::new_with_resources(
            service_name,
            Request::descriptor(),
            Response::descriptor(),
            resources,
        )
    }

    /// Constructs a slice-request/fixed-response descriptor with resources.
    pub fn for_slice_fixed_with_resources<Request, Response>(
        service_name: impl Into<String>,
        resources: RpcResourceConfig,
    ) -> Self
    where
        Request: SliceMessageAbi,
        Response: FixedMessageAbi,
    {
        Self::new_with_resources(
            service_name,
            Request::descriptor(),
            Response::descriptor(),
            resources,
        )
    }

    /// Constructs a slice-request/slice-response descriptor with resources.
    pub fn for_slice_with_resources<Request, Response>(
        service_name: impl Into<String>,
        resources: RpcResourceConfig,
    ) -> Self
    where
        Request: SliceMessageAbi,
        Response: SliceMessageAbi,
    {
        Self::new_with_resources(
            service_name,
            Request::descriptor(),
            Response::descriptor(),
            resources,
        )
    }

    /// Returns the transport service name.
    pub fn service_name(&self) -> &str {
        &self.service_name
    }

    /// Returns the complete request message descriptor.
    pub fn request_type(&self) -> &MessageTypeDescriptor {
        &self.request_type
    }

    /// Returns the complete response message descriptor.
    pub fn response_type(&self) -> &MessageTypeDescriptor {
        &self.response_type
    }

    /// Alias for [`RpcDescriptor::request_type`].
    pub fn request(&self) -> &MessageTypeDescriptor {
        self.request_type()
    }

    /// Alias for [`RpcDescriptor::response_type`].
    pub fn response(&self) -> &MessageTypeDescriptor {
        self.response_type()
    }

    /// Returns the advertised RPC transport capabilities.
    pub const fn transport(&self) -> RpcTransportFeatures {
        self.transport
    }

    /// Returns the advertised RPC resource ceilings.
    pub const fn resources(&self) -> RpcResourceConfig {
        self.resources
    }

    /// Returns the provider's advertised RPC readiness protocol.
    pub const fn readiness(&self) -> RpcReadinessProtocol {
        self.transport.readiness
    }
}

/// Marker for a type whose representation is safe to place in local shared
/// memory and read from another process.
///
/// # Safety
///
/// An implementation must guarantee all of the following for every value of
/// the type:
///
/// * the type is `Copy`, has no destructor, and contains no pointers,
///   references, slices, process-local handles, virtual tables, or other
///   address-bearing state;
/// * the type has a stable `#[repr(C)]` or `#[repr(transparent)]` layout whose
///   field order, sizes, alignment, and padding are intentionally part of its
///   ABI;
/// * every field is itself suitable for shared memory, and bytes written by a
///   producer can be read by an independently compiled consumer without
///   relying on Rust provenance or process-local allocation; and
/// * changing the representation or the meaning of any field requires a new
///   ABI major or layout signature.
///
/// This trait describes a local shared-memory ABI only.  It does not promise
/// that the type is suitable as a network wire format or that arbitrary bytes
/// are a valid value of the type.
pub unsafe trait ShmSafe: Copy + fmt::Debug + 'static {}

// SAFETY: These scalar values have fixed primitive representations on the
// supported little-endian targets and contain no process-local state.
unsafe impl ShmSafe for u8 {}
// SAFETY: These scalar values have fixed primitive representations on the
// supported little-endian targets and contain no process-local state.
unsafe impl ShmSafe for u16 {}
// SAFETY: These scalar values have fixed primitive representations on the
// supported little-endian targets and contain no process-local state.
unsafe impl ShmSafe for u32 {}
// SAFETY: These scalar values have fixed primitive representations on the
// supported little-endian targets and contain no process-local state.
unsafe impl ShmSafe for u64 {}
// SAFETY: These scalar values have fixed primitive representations on the
// supported little-endian targets and contain no process-local state.
unsafe impl ShmSafe for i8 {}
// SAFETY: These scalar values have fixed primitive representations on the
// supported little-endian targets and contain no process-local state.
unsafe impl ShmSafe for i16 {}
// SAFETY: These scalar values have fixed primitive representations on the
// supported little-endian targets and contain no process-local state.
unsafe impl ShmSafe for i32 {}
// SAFETY: These scalar values have fixed primitive representations on the
// supported little-endian targets and contain no process-local state.
unsafe impl ShmSafe for i64 {}
// SAFETY: IEEE-754 f32 has a fixed four-byte representation and all bit
// patterns are representable values; it contains no process-local state.
unsafe impl ShmSafe for f32 {}
// SAFETY: IEEE-754 f64 has a fixed eight-byte representation and all bit
// patterns are representable values; it contains no process-local state.
unsafe impl ShmSafe for f64 {}

/// Trait metadata for a fixed-size shared-memory message.
pub trait FixedMessageAbi: ShmSafe {
    /// Stable type namespace.
    const NAMESPACE: &'static str;
    /// Stable type name within the namespace.
    const NAME: &'static str;
    /// Major ABI version.
    const ABI_MAJOR: u16;
    /// Canonical field/type signature included in the layout hash.
    const SIGNATURE: &'static str;
    /// Optional descriptor generated from static ABI metadata.
    const DESCRIPTOR: Option<StaticMessageTypeDescriptor> = None;

    /// Builds the descriptor using this type's metadata and Rust's actual
    /// size/alignment values.
    fn descriptor() -> MessageTypeDescriptor
    where
        Self: Sized,
    {
        match Self::DESCRIPTOR {
            Some(descriptor) => descriptor.into_owned(),
            None => descriptor_for_fixed::<Self>(
                Self::NAMESPACE,
                Self::NAME,
                Self::ABI_MAJOR,
                Self::SIGNATURE,
            ),
        }
    }

    /// Alias for [`FixedMessageAbi::descriptor`].
    fn message_type_descriptor() -> MessageTypeDescriptor
    where
        Self: Sized,
    {
        Self::descriptor()
    }
}

/// Trait metadata for a header-plus-element-slice shared-memory message.
pub trait SliceMessageAbi: Copy + fmt::Debug + 'static {
    /// Fixed header type at the front of the message.
    type Header: ShmSafe;
    /// Repeated element type following the header.
    type Element: ShmSafe;

    /// Stable type namespace.
    const NAMESPACE: &'static str;
    /// Stable type name within the namespace.
    const NAME: &'static str;
    /// Major ABI version.
    const ABI_MAJOR: u16;
    /// Canonical header/element field signature included in the layout hash.
    const SIGNATURE: &'static str;
    /// Optional descriptor generated from static ABI metadata.
    const DESCRIPTOR: Option<StaticMessageTypeDescriptor> = None;

    /// Builds the descriptor using associated header/element layouts and Rust
    /// size/alignment values.
    fn descriptor() -> MessageTypeDescriptor
    where
        Self: Sized,
    {
        match Self::DESCRIPTOR {
            Some(descriptor) => descriptor.into_owned(),
            None => descriptor_for_slice::<Self::Header, Self::Element>(
                Self::NAMESPACE,
                Self::NAME,
                Self::ABI_MAJOR,
                Self::SIGNATURE,
            ),
        }
    }

    /// Alias for [`SliceMessageAbi::descriptor`].
    fn message_type_descriptor() -> MessageTypeDescriptor
    where
        Self: Sized,
    {
        Self::descriptor()
    }
}

fn descriptor_for_fixed<T: ShmSafe>(
    namespace: &str,
    name: &str,
    abi_major: u16,
    signature: &str,
) -> MessageTypeDescriptor {
    let header_size = abi_u32(mem::size_of::<T>());
    let header_align = abi_u32(mem::align_of::<T>());
    let element_size = 0;
    let element_align = 0;
    let canonical = canonical_layout_description(
        namespace,
        name,
        abi_major,
        MessageShape::Fixed,
        header_size,
        header_align,
        element_size,
        element_align,
        signature,
    );
    MessageTypeDescriptor::new(
        namespace,
        name,
        abi_major,
        MessageShape::Fixed,
        header_size,
        header_align,
        element_size,
        element_align,
        LayoutHash::from_description(&canonical),
    )
}

fn descriptor_for_slice<H: ShmSafe, E: ShmSafe>(
    namespace: &str,
    name: &str,
    abi_major: u16,
    signature: &str,
) -> MessageTypeDescriptor {
    let header_size = abi_u32(mem::size_of::<H>());
    let header_align = abi_u32(mem::align_of::<H>());
    let element_size = abi_u32(mem::size_of::<E>());
    let element_align = abi_u32(mem::align_of::<E>());
    let canonical = canonical_layout_description(
        namespace,
        name,
        abi_major,
        MessageShape::Slice,
        header_size,
        header_align,
        element_size,
        element_align,
        signature,
    );
    MessageTypeDescriptor::new(
        namespace,
        name,
        abi_major,
        MessageShape::Slice,
        header_size,
        header_align,
        element_size,
        element_align,
        LayoutHash::from_description(&canonical),
    )
}

fn abi_u32(value: usize) -> u32 {
    u32::try_from(value).expect("shared-memory ABI size/alignment exceeds u32")
}

// Frame, Detection, Image, PointCloud, and ControlCommand are generated from
// the checked-in `.rbt` schemas.  Keeping the includes explicit makes the
// schema-generated ABI visible and reviewable without introducing build-time
// code generation.
include!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../generated/rust/demo.rs"
));
include!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../generated/rust/sensor.rs"
));
include!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../generated/rust/control.rs"
));
include!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../generated/rust/tf.rs"
));

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, size_of};

    const GENERATED_FIXED_DESCRIPTOR: StaticMessageTypeDescriptor =
        StaticMessageTypeDescriptor::new(
            "generated.test",
            "Fixed",
            4,
            MessageShape::Fixed,
            99,
            7,
            0,
            0,
            LayoutHash::new([0x11; 16]),
        );
    const GENERATED_SLICE_DESCRIPTOR: StaticMessageTypeDescriptor =
        StaticMessageTypeDescriptor::new(
            "generated.test",
            "Slice",
            5,
            MessageShape::Slice,
            77,
            3,
            13,
            5,
            LayoutHash::new([0x22; 16]),
        );

    #[repr(C)]
    #[derive(Clone, Copy, Debug)]
    struct GeneratedFixed {
        value: u16,
    }

    // SAFETY: This test-only type is repr(C), Copy, and contains only a
    // fixed-width primitive value.
    unsafe impl ShmSafe for GeneratedFixed {}

    impl FixedMessageAbi for GeneratedFixed {
        const NAMESPACE: &'static str = "fallback.test";
        const NAME: &'static str = "GeneratedFixed";
        const ABI_MAJOR: u16 = 1;
        const SIGNATURE: &'static str = "value:u16@0";
        const DESCRIPTOR: Option<StaticMessageTypeDescriptor> = Some(GENERATED_FIXED_DESCRIPTOR);
    }

    #[repr(C)]
    #[derive(Clone, Copy, Debug)]
    struct GeneratedSlice;

    // SAFETY: This test-only marker is repr(C), Copy, and has no fields.
    unsafe impl ShmSafe for GeneratedSlice {}

    impl SliceMessageAbi for GeneratedSlice {
        type Header = u16;
        type Element = i8;

        const NAMESPACE: &'static str = "fallback.test";
        const NAME: &'static str = "GeneratedSlice";
        const ABI_MAJOR: u16 = 1;
        const SIGNATURE: &'static str = "header:u16@0;element:i8@0";
        const DESCRIPTOR: Option<StaticMessageTypeDescriptor> = Some(GENERATED_SLICE_DESCRIPTOR);
    }

    #[test]
    fn static_descriptor_converts_to_owned() {
        let owned = GENERATED_FIXED_DESCRIPTOR.to_owned();

        assert_eq!(
            MessageTypeDescriptor::from(GENERATED_FIXED_DESCRIPTOR),
            owned
        );
        assert_eq!(
            MessageTypeDescriptor::from(&GENERATED_FIXED_DESCRIPTOR),
            owned
        );
        assert_eq!(GENERATED_FIXED_DESCRIPTOR.into_owned(), owned);
        assert_eq!(owned.namespace, "generated.test");
        assert_eq!(owned.name, "Fixed");
        assert_eq!(owned.layout_hash, LayoutHash::new([0x11; 16]));
    }

    #[test]
    fn static_descriptors_override_fixed_and_slice_fallbacks() {
        let fixed = <GeneratedFixed as FixedMessageAbi>::descriptor();
        assert_eq!(fixed, GENERATED_FIXED_DESCRIPTOR.into_owned());
        assert_ne!(fixed.header_size, size_of::<GeneratedFixed>() as u32);

        let slice = <GeneratedSlice as SliceMessageAbi>::descriptor();
        assert_eq!(slice, GENERATED_SLICE_DESCRIPTOR.into_owned());
        assert_ne!(slice.header_size, size_of::<u16>() as u32);
        assert_ne!(slice.element_size, size_of::<i8>() as u32);
    }

    #[test]
    fn layout_hash_hex_and_serde_roundtrip() {
        let hash = LayoutHash::from_description("test-layout");
        let hex = hash.to_string();
        assert_eq!(hex.len(), 32);
        assert!(hex.bytes().all(|byte| byte.is_ascii_hexdigit()));
        assert_eq!(format!("{hash:?}"), hex);
        assert_eq!(hex, hex.to_ascii_lowercase());
        assert_eq!(hex.parse::<LayoutHash>().unwrap(), hash);
        assert_eq!(
            hex.to_ascii_uppercase().parse::<LayoutHash>().unwrap(),
            hash
        );
        assert!(matches!(
            "00".parse::<LayoutHash>(),
            Err(LayoutHashParseError::InvalidLength { .. })
        ));
        assert!(matches!(
            "0000000000000000000000000000000g".parse::<LayoutHash>(),
            Err(LayoutHashParseError::InvalidCharacter { .. })
        ));

        let json = serde_json::to_string(&hash).unwrap();
        assert_eq!(json, format!("\"{hex}\""));
        assert_eq!(serde_json::from_str::<LayoutHash>(&json).unwrap(), hash);
    }

    #[test]
    fn layout_hash_changes_with_signature_but_not_for_same_description() {
        let first = canonical_layout_description(
            "test",
            "Message",
            1,
            MessageShape::Fixed,
            16,
            8,
            0,
            0,
            "a:u64;b:u64",
        );
        let same = canonical_layout_description(
            "test",
            "Message",
            1,
            MessageShape::Fixed,
            16,
            8,
            0,
            0,
            "a:u64;b:u64",
        );
        let different = canonical_layout_description(
            "test",
            "Message",
            1,
            MessageShape::Fixed,
            16,
            8,
            0,
            0,
            "a:u64;b:u32",
        );
        assert_eq!(
            LayoutHash::from_description(&first),
            LayoutHash::from_description(&same)
        );
        assert_ne!(
            LayoutHash::from_description(&first),
            LayoutHash::from_description(&different)
        );
    }

    #[test]
    fn descriptor_version_is_separate_from_generator_metadata() {
        assert_eq!(MESSAGE_DESCRIPTOR_VERSION, 1);
        let description = canonical_layout_description(
            "test",
            "Message",
            1,
            MessageShape::Fixed,
            16,
            8,
            0,
            0,
            "value:u64@0",
        );
        assert!(description.contains("version=1\n"));
        assert!(!description.contains("rbt-idl"));
        assert!(!description.contains("sdk"));
        assert!(!description.contains(env!("CARGO_PKG_VERSION")));
    }

    #[test]
    fn built_in_sizes_and_alignments_are_stable() {
        assert_eq!(size_of::<Frame>(), 16);
        assert_eq!(align_of::<Frame>(), 8);
        assert_eq!(size_of::<Detection>(), 16);
        assert_eq!(align_of::<Detection>(), 8);
        assert_eq!(size_of::<PixelFormat>(), 4);
        assert_eq!(align_of::<PixelFormat>(), 4);
        assert_eq!(size_of::<ImageHeader>(), 32);
        assert_eq!(align_of::<ImageHeader>(), 8);
        assert_eq!(size_of::<PointCloudHeader>(), 32);
        assert_eq!(align_of::<PointCloudHeader>(), 8);
        assert_eq!(size_of::<PointXYZI>(), 16);
        assert_eq!(align_of::<PointXYZI>(), 4);
        assert_eq!(size_of::<ControlCommand>(), 16);
        assert_eq!(align_of::<ControlCommand>(), 8);

        let frame = Frame::descriptor();
        assert_eq!(frame.shape, MessageShape::Fixed);
        assert_eq!((frame.header_size, frame.header_align), (16, 8));
        assert_eq!((frame.element_size, frame.element_align), (0, 0));
        let image = Image::descriptor();
        assert_eq!(image.shape, MessageShape::Slice);
        assert_eq!((image.header_size, image.header_align), (32, 8));
        assert_eq!((image.element_size, image.element_align), (1, 1));
        let cloud = PointCloud::descriptor();
        assert_eq!(cloud.shape, MessageShape::Slice);
        assert_eq!((cloud.header_size, cloud.header_align), (32, 8));
        assert_eq!((cloud.element_size, cloud.element_align), (16, 4));
    }

    #[test]
    fn descriptors_and_streams_roundtrip_over_json() {
        let stream = StreamDescriptor::for_fixed::<Frame>("camera.frames", StreamProfile::Sensor);
        let json = serde_json::to_string(&stream).unwrap();
        let restored: StreamDescriptor = serde_json::from_str(&json).unwrap();
        assert_eq!(restored, stream);
        assert_eq!(restored.service_name(), "camera.frames");
        assert_eq!(restored.message_type().qualified_name(), "rbt.demo/Frame@1");
        assert_eq!(restored.profile(), StreamProfile::Sensor);
        assert_eq!(restored.readiness(), ReadinessProtocol::SidecarV1);
        assert!(json.contains("\"readiness\":\"sidecar-v1\""));
    }

    #[test]
    fn stream_resource_defaults_and_layout_hash_invariance_are_stable() {
        assert_eq!(
            StreamResourceConfig::default(),
            StreamResourceConfig::new(2, 1)
        );

        let conservative = StreamDescriptor::for_fixed_with_resources::<Frame>(
            "camera.frames",
            StreamProfile::Sensor,
            StreamResourceConfig::new(2, 1),
        );
        let expanded = StreamDescriptor::for_fixed_with_resources::<Frame>(
            "camera.frames",
            StreamProfile::Sensor,
            StreamResourceConfig::new(8, 4),
        );

        assert_ne!(conservative.resources(), expanded.resources());
        assert_eq!(
            conservative.message_type().layout_hash,
            expanded.message_type().layout_hash
        );
        assert_eq!(
            conservative.message_type().layout_hash,
            Frame::descriptor().layout_hash
        );
    }

    #[test]
    fn legacy_stream_descriptor_defaults_to_no_readiness_feature() {
        let current = StreamDescriptor::for_fixed::<Frame>("camera.frames", StreamProfile::Sensor);
        let mut value = serde_json::to_value(current).unwrap();
        value.as_object_mut().unwrap().remove("transport");

        let restored: StreamDescriptor = serde_json::from_value(value).unwrap();

        assert_eq!(restored.readiness(), ReadinessProtocol::None);
    }

    #[test]
    fn rpc_fixed_and_slice_constructors_use_ordinary_descriptors() {
        let fixed_fixed = RpcDescriptor::for_fixed::<Frame, ControlCommand>("control.rpc");
        assert_eq!(fixed_fixed.service_name(), "control.rpc");
        assert_eq!(fixed_fixed.request_type(), &Frame::descriptor());
        assert_eq!(fixed_fixed.response_type(), &ControlCommand::descriptor());
        assert!(fixed_fixed.request_type().is_fixed());
        assert!(fixed_fixed.response_type().is_fixed());

        let fixed_slice = RpcDescriptor::for_fixed_slice::<Frame, Image>("frame-image.rpc");
        assert_eq!(fixed_slice.request_type(), &Frame::descriptor());
        assert_eq!(fixed_slice.response_type(), &Image::descriptor());
        assert!(fixed_slice.request_type().is_fixed());
        assert!(fixed_slice.response_type().is_slice());

        let slice_fixed = RpcDescriptor::for_slice_fixed::<Image, Frame>("image-frame.rpc");
        assert_eq!(slice_fixed.request_type(), &Image::descriptor());
        assert_eq!(slice_fixed.response_type(), &Frame::descriptor());
        assert!(slice_fixed.request_type().is_slice());
        assert!(slice_fixed.response_type().is_fixed());

        let slice_slice = RpcDescriptor::for_slice::<Image, PointCloud>("image-cloud.rpc");
        assert_eq!(slice_slice.request(), &Image::descriptor());
        assert_eq!(slice_slice.response(), &PointCloud::descriptor());
        assert!(slice_slice.request().is_slice());
        assert!(slice_slice.response().is_slice());

        assert_eq!(fixed_fixed.readiness(), RpcReadinessProtocol::RpcSidecarV1);
        assert_eq!(fixed_fixed.resources(), RpcResourceConfig::default());
        assert_eq!(
            fixed_fixed.request_type().layout_hash,
            Frame::descriptor().layout_hash
        );
        assert_eq!(
            fixed_fixed.response_type().layout_hash,
            ControlCommand::descriptor().layout_hash
        );
    }

    #[test]
    fn rpc_descriptor_serde_roundtrip_and_legacy_readiness_default() {
        let descriptor = RpcDescriptor::for_fixed::<Frame, ControlCommand>("control.rpc");
        let json = serde_json::to_string(&descriptor).unwrap();
        assert!(json.contains("\"readiness\":\"rpc-sidecar-v1\""));
        let restored: RpcDescriptor = serde_json::from_str(&json).unwrap();
        assert_eq!(restored, descriptor);

        let mut partial = serde_json::to_value(&descriptor).unwrap();
        partial
            .get_mut("transport")
            .and_then(serde_json::Value::as_object_mut)
            .unwrap()
            .remove("readiness");
        let restored: RpcDescriptor = serde_json::from_value(partial).unwrap();
        assert_eq!(restored.readiness(), RpcReadinessProtocol::None);

        let mut legacy = serde_json::to_value(&descriptor).unwrap();
        legacy.as_object_mut().unwrap().remove("transport");
        let restored: RpcDescriptor = serde_json::from_value(legacy).unwrap();
        assert_eq!(restored.readiness(), RpcReadinessProtocol::None);

        let rpc_readiness: RpcReadinessProtocol =
            serde_json::from_str("\"rpc-sidecar-v1\"").unwrap();
        assert_eq!(rpc_readiness, RpcReadinessProtocol::RpcSidecarV1);
        assert_eq!(rpc_readiness.to_string(), "rpc-sidecar-v1");
    }

    #[test]
    fn rpc_resource_defaults_are_bounded_and_partial_json_is_compatible() {
        let defaults = RpcResourceConfig::default();
        assert_eq!(defaults, RpcResourceConfig::new(4, 16, 2, 1, 1, 1, 1, 1));
        assert_eq!(defaults.max_in_flight(), 4);
        assert_eq!(defaults.max_clients(), 16);
        assert_eq!(defaults.max_request_loans(), 2);
        assert_eq!(defaults.response_buffer_size(), 1);
        assert_eq!(defaults.max_borrowed_responses(), 1);
        assert_eq!(defaults.max_response_loans(), 1);
        assert_eq!(defaults.max_request_slice_len(), 1);
        assert_eq!(defaults.max_response_slice_len(), 1);
        assert_eq!(
            RpcTransportFeatures::default().readiness(),
            RpcReadinessProtocol::None
        );
        assert_eq!(
            RpcTransportFeatures::rpc_sidecar_v1().readiness(),
            RpcReadinessProtocol::RpcSidecarV1
        );

        let partial: RpcResourceConfig = serde_json::from_str(r#"{"max_clients": 3}"#).unwrap();
        assert_eq!(partial.max_clients, 3);
        assert_eq!(partial.max_in_flight, 4);
        assert_eq!(partial.max_request_loans, 2);
        assert_eq!(partial.response_buffer_size, 1);
        assert_eq!(partial.max_borrowed_responses, 1);
        assert_eq!(partial.max_response_loans, 1);
        assert_eq!(partial.max_request_slice_len, 1);
        assert_eq!(partial.max_response_slice_len, 1);

        let customized = defaults
            .with_max_request_slice_len(8)
            .with_max_response_slice_len(12);
        assert_eq!(customized.max_request_slice_len, 8);
        assert_eq!(customized.max_response_slice_len, 12);
    }

    #[test]
    fn built_in_layout_hashes_are_fixed() {
        assert_eq!(
            Frame::descriptor().layout_hash.to_string(),
            "5d2c102a797d5b0ab64b4db8844297c0"
        );
        assert_eq!(
            Detection::descriptor().layout_hash.to_string(),
            "d8e60d2ca65e6b5b74f0981d34884a4d"
        );
        assert_eq!(
            Image::descriptor().layout_hash.to_string(),
            "943fa4008cf0f55f89a281b051c69a6f"
        );
        assert_eq!(
            PointCloud::descriptor().layout_hash.to_string(),
            "d9da2bb29bb2cf217c40f1a56d1fb755"
        );
        assert_eq!(
            ControlCommand::descriptor().layout_hash.to_string(),
            "9381cf15842a48ac0964111f0f45ef72"
        );
    }

    #[test]
    fn tf_layout_hashes_are_fixed() {
        assert_eq!(
            DynamicTransform::descriptor().layout_hash.to_string(),
            "b1a02959970fa0ecd30b6219ab2bcb56"
        );
        assert_eq!(
            StaticSnapshotRequest::descriptor().layout_hash.to_string(),
            "85b655e27004d7d38eb10d720e035d7b"
        );
        assert_eq!(
            StaticSnapshot::descriptor().layout_hash.to_string(),
            "cd373f3a4bdf5d680aec5d208da98e82"
        );
        assert_eq!(
            StaticUpdate::descriptor().layout_hash.to_string(),
            "8e4aaeb6d5c1bd7c040a9ff2384432b8"
        );
    }

    #[test]
    fn tf_large_fixed_arrays_roundtrip_in_canonical_field_order() {
        let frame_id = TfFrameId {
            bytes: std::array::from_fn(|index| index as u8),
        };
        let mut frame_bytes = Vec::new();
        frame_id.encode_canonical(&mut frame_bytes);
        assert_eq!(frame_bytes, (0_u8..64).collect::<Vec<_>>());

        let mut decoded_frame_id = TfFrameId::default();
        let mut cursor = CanonicalCursor::new(&frame_bytes);
        decoded_frame_id.decode_canonical(&mut cursor).unwrap();
        cursor.finish(frame_bytes.len()).unwrap();
        assert_eq!(decoded_frame_id.bytes, frame_id.bytes);

        let timestamp = TfTimestamp {
            kind: 7,
            flags: 1,
            reserved: [1, 2, 3, 4, 5, 6],
            source_id: std::array::from_fn(|index| 255 - index as u8),
            generation: 0x0102_0304_0506_0708,
            nanoseconds: -123_456_789,
            uncertainty_ns: 987_654_321,
        };
        let mut timestamp_bytes = Vec::new();
        timestamp.encode_canonical(&mut timestamp_bytes);
        let mut expected_timestamp_bytes = vec![timestamp.kind, timestamp.flags];
        expected_timestamp_bytes.extend_from_slice(&timestamp.reserved);
        expected_timestamp_bytes.extend_from_slice(&timestamp.source_id);
        expected_timestamp_bytes.extend_from_slice(&timestamp.generation.to_le_bytes());
        expected_timestamp_bytes.extend_from_slice(&timestamp.nanoseconds.to_le_bytes());
        expected_timestamp_bytes.extend_from_slice(&timestamp.uncertainty_ns.to_le_bytes());
        assert_eq!(timestamp_bytes, expected_timestamp_bytes);

        let mut decoded_timestamp = TfTimestamp::default();
        let mut cursor = CanonicalCursor::new(&timestamp_bytes);
        decoded_timestamp.decode_canonical(&mut cursor).unwrap();
        cursor.finish(timestamp_bytes.len()).unwrap();
        assert_eq!(decoded_timestamp.kind, timestamp.kind);
        assert_eq!(decoded_timestamp.flags, timestamp.flags);
        assert_eq!(decoded_timestamp.reserved, timestamp.reserved);
        assert_eq!(decoded_timestamp.source_id, timestamp.source_id);
        assert_eq!(decoded_timestamp.generation, timestamp.generation);
        assert_eq!(decoded_timestamp.nanoseconds, timestamp.nanoseconds);
        assert_eq!(decoded_timestamp.uncertainty_ns, timestamp.uncertainty_ns);
    }
}

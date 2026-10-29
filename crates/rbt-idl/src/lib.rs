//! A small, dependency-light IDL compiler for the shared-memory ABI.
//!
//! The compiler deliberately has three representations: [`AstSchema`] keeps
//! the spelling and source locations produced by the parser, [`ResolvedSchema`]
//! contains checked type references and target-independent layouts, and
//! [`CanonicalSchema`] is the stable model used by the hash and generators.
//! No compiler output is derived from comments or insignificant whitespace.

#![forbid(unsafe_code)]

use std::{
    collections::{BTreeMap, BTreeSet, HashMap},
    fmt, fs,
    path::{Path, PathBuf},
};

use serde::Serialize;
use thiserror::Error;

/// The only target currently supported by this local ABI.
pub const TARGET_ENDIANNESS: &str = "little";
/// Version of the source IDL language understood by this compiler.
pub const IDL_LANGUAGE_VERSION: u16 = 1;
/// Version of the canonical message descriptor format.
pub const MESSAGE_DESCRIPTOR_VERSION: u16 = 1;
/// Backwards-compatible name for [`MESSAGE_DESCRIPTOR_VERSION`].
#[deprecated(note = "use MESSAGE_DESCRIPTOR_VERSION instead")]
pub const CANONICAL_VERSION: u16 = MESSAGE_DESCRIPTOR_VERSION;
/// Canonical layout domain, shared with `rbt-types`.
pub const CANONICAL_DOMAIN: &str = "rbt-shm-abi";

/// A source location used in parser and semantic diagnostics.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct SourceLocation {
    pub filename: String,
    pub line: usize,
    pub column: usize,
}

impl SourceLocation {
    fn new(filename: &str, line: usize, column: usize) -> Self {
        Self {
            filename: filename.to_owned(),
            line,
            column,
        }
    }
}

impl fmt::Display for SourceLocation {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}:{}:{}", self.filename, self.line, self.column)
    }
}

fn diagnostic_message(
    source: &str,
    location: &SourceLocation,
    message: impl Into<String>,
) -> String {
    let message = message.into();
    let Some(line) = source.split('\n').nth(location.line.saturating_sub(1)) else {
        return message;
    };
    let line = line.strip_suffix('\r').unwrap_or(line);
    let column = location.column.saturating_sub(1).min(line.len());
    let column = (0..=column)
        .rev()
        .find(|index| line.is_char_boundary(*index))
        .unwrap_or(0);
    let caret = line[..column].chars().count();
    let line_number = location.line.to_string();
    let gutter = " ".repeat(line_number.len());
    format!(
        "{message}\n  |\n{line_number} | {line}\n{gutter} | {}^",
        " ".repeat(caret)
    )
}

fn parse_error(source: &str, location: SourceLocation, message: impl Into<String>) -> Error {
    Error::Parse {
        message: diagnostic_message(source, &location, message),
        location,
    }
}

/// An error emitted by lexing, parsing, semantic validation, or output.
#[derive(Debug, Error)]
pub enum Error {
    #[error("{location}: {message}")]
    Lex {
        location: SourceLocation,
        message: String,
    },
    #[error("{location}: {message}")]
    Parse {
        location: SourceLocation,
        message: String,
    },
    #[error("{location}: {message}")]
    Semantic {
        location: SourceLocation,
        message: String,
    },
    #[error("{path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("{path}: {source}")]
    Json {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
}

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Clone, Debug, Eq, PartialEq)]
enum TokenKind {
    Ident(String),
    Integer(String),
    Symbol(char),
    Eof,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Token {
    kind: TokenKind,
    location: SourceLocation,
}

fn lex(source: &str, filename: &str) -> Result<Vec<Token>> {
    let mut tokens = Vec::new();
    let bytes = source.as_bytes();
    let mut index = 0;
    let mut line = 1;
    let mut column = 1;
    while index < bytes.len() {
        let byte = bytes[index];
        if byte.is_ascii_whitespace() {
            if byte == b'\n' {
                line += 1;
                column = 1;
            } else {
                column += 1;
            }
            index += 1;
            continue;
        }
        if byte == b'/' && bytes.get(index + 1) == Some(&b'/') {
            index += 2;
            column += 2;
            while index < bytes.len() && bytes[index] != b'\n' {
                index += 1;
                column += 1;
            }
            continue;
        }
        if byte == b'/' && bytes.get(index + 1) == Some(&b'*') {
            let start = SourceLocation::new(filename, line, column);
            index += 2;
            column += 2;
            let mut terminated = false;
            while index < bytes.len() {
                if bytes[index] == b'*' && bytes.get(index + 1) == Some(&b'/') {
                    index += 2;
                    column += 2;
                    terminated = true;
                    break;
                }
                if bytes[index] == b'\n' {
                    index += 1;
                    line += 1;
                    column = 1;
                } else {
                    index += 1;
                    column += 1;
                }
            }
            if !terminated {
                return Err(Error::Lex {
                    message: diagnostic_message(source, &start, "unterminated block comment"),
                    location: start,
                });
            }
            continue;
        }
        let location = SourceLocation::new(filename, line, column);
        if byte.is_ascii_alphabetic() || byte == b'_' {
            let start = index;
            while index < bytes.len()
                && (bytes[index].is_ascii_alphanumeric() || bytes[index] == b'_')
            {
                index += 1;
                column += 1;
            }
            tokens.push(Token {
                kind: TokenKind::Ident(source[start..index].to_owned()),
                location,
            });
            continue;
        }
        if byte.is_ascii_digit()
            || (byte == b'-' && bytes.get(index + 1).is_some_and(u8::is_ascii_digit))
        {
            let start = index;
            if byte == b'-' {
                index += 1;
                column += 1;
            }
            while index < bytes.len() && bytes[index].is_ascii_digit() {
                index += 1;
                column += 1;
            }
            tokens.push(Token {
                kind: TokenKind::Integer(source[start..index].to_owned()),
                location,
            });
            continue;
        }
        if ".,;:{}[]()=@".contains(byte as char) {
            tokens.push(Token {
                kind: TokenKind::Symbol(byte as char),
                location,
            });
            index += 1;
            column += 1;
            continue;
        }
        return Err(Error::Lex {
            message: diagnostic_message(
                source,
                &location,
                format!("unexpected character {:?}", byte as char),
            ),
            location,
        });
    }
    tokens.push(Token {
        kind: TokenKind::Eof,
        location: SourceLocation::new(filename, line, column),
    });
    Ok(tokens)
}

/// A type spelling in the parsed AST.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum AstType {
    Named(String),
    Array { element: Box<AstType>, length: u32 },
    Dynamic(Box<AstType>),
}

impl AstType {
    pub fn named(name: impl Into<String>) -> Self {
        Self::Named(name.into())
    }

    pub fn array(element: Self, length: u32) -> Self {
        Self::Array {
            element: Box::new(element),
            length,
        }
    }

    pub fn dynamic(element: Self) -> Self {
        Self::Dynamic(Box::new(element))
    }
}

/// An AST field with its declaration location.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AstField {
    pub name: String,
    pub ty: AstType,
    pub location: SourceLocation,
}

/// An integer constant in a primitive newtype declaration.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AstConstant {
    pub name: String,
    pub value: i128,
    pub location: SourceLocation,
}

/// A primitive declaration.  A declaration with constants is a transparent
/// newtype; a declaration without constants names an integer primitive.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AstPrimitive {
    pub name: String,
    pub underlying: Option<String>,
    pub constants: Vec<AstConstant>,
    pub location: SourceLocation,
}

/// A fixed-layout struct declaration.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AstStruct {
    pub name: String,
    pub fields: Vec<AstField>,
    pub location: SourceLocation,
}

/// The two message shapes accepted by the ABI.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum AstMessageShape {
    Fixed { fields: Vec<AstField> },
    Slice { header: AstType, payload: AstType },
}

/// A message declaration.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AstMessage {
    pub name: String,
    pub shape: AstMessageShape,
    pub location: SourceLocation,
}

/// One top-level declaration in source order.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum AstDeclaration {
    Primitive(AstPrimitive),
    Struct(AstStruct),
    Message(AstMessage),
}

impl AstDeclaration {
    pub fn name(&self) -> &str {
        match self {
            Self::Primitive(value) => &value.name,
            Self::Struct(value) => &value.name,
            Self::Message(value) => &value.name,
        }
    }

    fn location(&self) -> &SourceLocation {
        match self {
            Self::Primitive(value) => &value.location,
            Self::Struct(value) => &value.location,
            Self::Message(value) => &value.location,
        }
    }
}

/// The parser output.  It intentionally retains source locations and does
/// not resolve names or calculate a target layout.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AstSchema {
    pub filename: String,
    pub package: String,
    pub abi_major: u16,
    pub declarations: Vec<AstDeclaration>,
    source: String,
}

pub type SchemaAst = AstSchema;

struct Parser<'source> {
    tokens: Vec<Token>,
    index: usize,
    filename: String,
    source: &'source str,
}

impl<'source> Parser<'source> {
    fn new(tokens: Vec<Token>, filename: &str, source: &'source str) -> Self {
        Self {
            tokens,
            index: 0,
            filename: filename.to_owned(),
            source,
        }
    }

    fn current(&self) -> &Token {
        &self.tokens[self.index]
    }

    fn peek_kind(&self, offset: usize) -> &TokenKind {
        &self.tokens[(self.index + offset).min(self.tokens.len() - 1)].kind
    }

    fn advance(&mut self) -> Token {
        let token = self.tokens[self.index].clone();
        self.index = (self.index + 1).min(self.tokens.len() - 1);
        token
    }

    fn error<T>(&self, message: impl Into<String>) -> Result<T> {
        Err(parse_error(
            self.source,
            self.current().location.clone(),
            message,
        ))
    }

    fn error_at<T>(&self, location: SourceLocation, message: impl Into<String>) -> Result<T> {
        Err(parse_error(self.source, location, message))
    }

    fn expect_ident(&mut self, what: &str) -> Result<(String, SourceLocation)> {
        match self.advance() {
            Token {
                kind: TokenKind::Ident(value),
                location,
            } => Ok((value, location)),
            token => self.error(format!(
                "expected {what}, got {}",
                token_description(&token.kind)
            )),
        }
    }

    fn eat_ident(&mut self, value: &str) -> bool {
        match self.peek_kind(0) {
            TokenKind::Ident(actual) if actual == value => {
                self.advance();
                true
            }
            _ => false,
        }
    }

    fn expect_symbol(&mut self, symbol: char) -> Result<SourceLocation> {
        match self.advance() {
            Token {
                kind: TokenKind::Symbol(actual),
                location,
            } if actual == symbol => Ok(location),
            token => self.error(format!(
                "expected {:?}, got {}",
                symbol,
                token_description(&token.kind)
            )),
        }
    }

    fn eat_symbol(&mut self, symbol: char) -> bool {
        match self.peek_kind(0) {
            TokenKind::Symbol(actual) if *actual == symbol => {
                self.advance();
                true
            }
            _ => false,
        }
    }

    fn expect_integer(&mut self, what: &str) -> Result<(String, SourceLocation)> {
        match self.advance() {
            Token {
                kind: TokenKind::Integer(value),
                location,
            } => Ok((value, location)),
            token => self.error(format!(
                "expected {what}, got {}",
                token_description(&token.kind)
            )),
        }
    }

    fn parse(mut self) -> Result<AstSchema> {
        let mut package = None;
        let mut abi_major = None;
        let mut declarations = Vec::new();
        while !matches!(self.peek_kind(0), TokenKind::Eof) {
            let (keyword, keyword_location) = self.expect_ident("a top-level declaration")?;
            match keyword.as_str() {
                "package" => {
                    if package.is_some() {
                        return self.error_at(keyword_location, "duplicate package declaration");
                    }
                    package = Some(self.parse_qualified_name("package name")?);
                    self.eat_symbol(';');
                }
                "abi" | "abi_major" => {
                    if abi_major.is_some() {
                        return self.error_at(keyword_location, "duplicate abi declaration");
                    }
                    self.eat_symbol('=');
                    let (value, location) = self.expect_integer("ABI major version")?;
                    let parsed = value.parse::<u64>().map_err(|_| {
                        parse_error(
                            self.source,
                            location.clone(),
                            "ABI major version must be a non-negative integer",
                        )
                    })?;
                    let parsed = u16::try_from(parsed).map_err(|_| {
                        parse_error(
                            self.source,
                            location.clone(),
                            "ABI major version does not fit u16",
                        )
                    })?;
                    abi_major = Some(parsed);
                    self.eat_symbol(';');
                }
                "primitive" | "newtype" => declarations.push(AstDeclaration::Primitive(
                    self.parse_primitive(keyword_location)?,
                )),
                "struct" => {
                    declarations.push(AstDeclaration::Struct(self.parse_struct(keyword_location)?))
                }
                "message" => declarations.push(AstDeclaration::Message(
                    self.parse_message(keyword_location)?,
                )),
                "slice" => declarations.push(AstDeclaration::Message(
                    self.parse_slice_declaration(keyword_location)?,
                )),
                _ => {
                    return self.error_at(
                        keyword_location,
                        format!("unexpected top-level keyword {keyword:?}"),
                    );
                }
            }
        }
        let package = package.ok_or_else(|| {
            parse_error(
                self.source,
                SourceLocation::new(&self.filename, 1, 1),
                "missing package declaration",
            )
        })?;
        let abi_major = abi_major.ok_or_else(|| {
            parse_error(
                self.source,
                SourceLocation::new(&self.filename, 1, 1),
                "missing abi declaration",
            )
        })?;
        Ok(AstSchema {
            filename: self.filename,
            package,
            abi_major,
            declarations,
            source: self.source.to_owned(),
        })
    }

    fn parse_qualified_name(&mut self, what: &str) -> Result<String> {
        let (first, _) = self.expect_ident(what)?;
        let mut value = first;
        while self.eat_symbol('.') {
            let (part, _) = self.expect_ident(what)?;
            value.push('.');
            value.push_str(&part);
        }
        Ok(value)
    }

    fn parse_primitive(&mut self, location: SourceLocation) -> Result<AstPrimitive> {
        let (name, _) = self.expect_ident("primitive name")?;
        let mut underlying = None;
        let has_separator = self.eat_symbol(':') || self.eat_symbol('=');
        if has_separator
            || (matches!(self.peek_kind(0), TokenKind::Ident(_))
                && !matches!(
                    self.peek_kind(1),
                    TokenKind::Symbol(';') | TokenKind::Symbol('{')
                ))
        {
            let (base, _) = self.expect_ident("primitive underlying type")?;
            underlying = Some(base);
        }
        let mut constants = Vec::new();
        if self.eat_symbol('{') {
            while !self.eat_symbol('}') {
                if matches!(self.peek_kind(0), TokenKind::Eof) {
                    return self.error("unterminated primitive declaration");
                }
                let (constant_name, constant_location) = self.expect_ident("constant name")?;
                self.expect_symbol('=')?;
                let (raw, value_location) = self.expect_integer("integer constant")?;
                let value = raw.parse::<i128>().map_err(|_| {
                    parse_error(
                        self.source,
                        value_location.clone(),
                        "integer constant is out of range",
                    )
                })?;
                constants.push(AstConstant {
                    name: constant_name,
                    value,
                    location: constant_location,
                });
                if !self.eat_symbol(';') {
                    self.eat_symbol(',');
                }
            }
            self.eat_symbol(';');
        } else {
            self.expect_symbol(';')?;
        }
        Ok(AstPrimitive {
            name,
            underlying,
            constants,
            location,
        })
    }

    fn parse_struct(&mut self, location: SourceLocation) -> Result<AstStruct> {
        let (name, _) = self.expect_ident("struct name")?;
        let fields = self.parse_fields()?;
        Ok(AstStruct {
            name,
            fields,
            location,
        })
    }

    fn parse_message(&mut self, location: SourceLocation) -> Result<AstMessage> {
        let (name, _) = self.expect_ident("message name")?;
        let has_separator = self.eat_symbol('=') || self.eat_symbol(':');
        if has_separator && self.eat_ident("slice") {
            self.expect_symbol('{')?;
            let (header, payload) = self.parse_slice_body_after_open()?;
            return Ok(AstMessage {
                name,
                shape: AstMessageShape::Slice { header, payload },
                location,
            });
        }
        if matches!(self.peek_kind(0), TokenKind::Symbol('{')) {
            let fields = self.parse_fields()?;
            return Ok(AstMessage {
                name,
                shape: AstMessageShape::Fixed { fields },
                location,
            });
        }
        if has_separator {
            self.error("expected `slice` after message shape separator")
        } else {
            self.error("expected message body")
        }
    }

    fn parse_slice_declaration(&mut self, location: SourceLocation) -> Result<AstMessage> {
        let (name, _) = self.expect_ident("slice message name")?;
        self.eat_symbol('=');
        self.eat_ident("slice");
        self.expect_symbol('{')?;
        let (header, payload) = self.parse_slice_body_after_open()?;
        Ok(AstMessage {
            name,
            shape: AstMessageShape::Slice { header, payload },
            location,
        })
    }

    fn parse_slice_body_after_open(&mut self) -> Result<(AstType, AstType)> {
        let mut header = None;
        let mut payload = None;
        while !self.eat_symbol('}') {
            let (label, location) = self.expect_ident("slice header or payload")?;
            self.eat_symbol(':');
            self.eat_symbol('=');
            let ty = self.parse_type()?;
            if label == "header" {
                if header.is_some() {
                    return self.error_at(location, "duplicate slice header");
                }
                header = Some(ty);
            } else if label == "payload" || label == "element" {
                if payload.is_some() {
                    return self.error_at(location, "duplicate slice payload");
                }
                payload = Some(ty);
            } else {
                return self.error_at(
                    location,
                    format!("slice body only accepts header and payload, got {label:?}"),
                );
            }
            self.eat_symbol(';');
            self.eat_symbol(',');
        }
        self.eat_symbol(';');
        let header = header.ok_or_else(|| {
            parse_error(
                self.source,
                self.current().location.clone(),
                "slice message is missing header",
            )
        })?;
        let payload = payload.ok_or_else(|| {
            parse_error(
                self.source,
                self.current().location.clone(),
                "slice message is missing payload",
            )
        })?;
        Ok((header, payload))
    }

    fn parse_fields(&mut self) -> Result<Vec<AstField>> {
        self.expect_symbol('{')?;
        self.parse_fields_after_open()
    }

    fn parse_fields_after_open(&mut self) -> Result<Vec<AstField>> {
        let mut fields = Vec::new();
        while !self.eat_symbol('}') {
            if matches!(self.peek_kind(0), TokenKind::Eof) {
                return self.error("unterminated field block");
            }
            let first = self.expect_ident("field name or type")?;
            let (name, ty) = if self.eat_symbol(':') {
                (first.0, self.parse_type()?)
            } else if matches!(self.peek_kind(0), TokenKind::Ident(_)) {
                // Both `name: Type` and the compact C-like `Type name`
                // spelling are accepted.  Built-ins and conventional
                // upper-case type names make the otherwise ambiguous form
                // deterministic; unknown/unknown pairs default to
                // `name Type`, which is the documented spelling.
                if is_likely_type_name(&first.0) {
                    let ty = self.parse_type_starting_with(first.0)?;
                    let (name, _) = self.expect_ident("field name")?;
                    (name, ty)
                } else {
                    let name = first.0;
                    let (type_name, _) = self.expect_ident("field type")?;
                    (name, self.parse_type_starting_with(type_name)?)
                }
            } else if matches!(self.peek_kind(0), TokenKind::Symbol('[')) {
                let ty = self.parse_type_starting_with(first.0)?;
                let (name, _) = self.expect_ident("field name")?;
                (name, ty)
            } else {
                return self.error_at(first.1, "expected ':' or a field type after field name");
            };
            fields.push(AstField {
                name,
                ty,
                location: first.1,
            });
            if !self.eat_symbol(';') {
                self.eat_symbol(',');
            }
        }
        self.eat_symbol(';');
        Ok(fields)
    }

    fn parse_type(&mut self) -> Result<AstType> {
        if self.eat_symbol('[') {
            let element = self.parse_type()?;
            if !self.eat_symbol(';') {
                self.eat_symbol(',');
            }
            let (raw, location) = self.expect_integer("array length")?;
            let length = parse_array_length(&raw, &location, self.source)?;
            self.expect_symbol(']')?;
            return Ok(AstType::Array {
                element: Box::new(element),
                length,
            });
        }
        let (name, _) = self.expect_ident("type name")?;
        self.parse_type_starting_with(name)
    }

    fn parse_type_starting_with(&mut self, name: String) -> Result<AstType> {
        let mut ty = AstType::Named(name);
        loop {
            if !self.eat_symbol('[') {
                break;
            }
            if self.eat_symbol(']') {
                ty = AstType::Dynamic(Box::new(ty));
            } else {
                let (raw, location) = self.expect_integer("array length")?;
                let length = parse_array_length(&raw, &location, self.source)?;
                self.expect_symbol(']')?;
                ty = AstType::Array {
                    element: Box::new(ty),
                    length,
                };
            }
        }
        Ok(ty)
    }
}

fn parse_array_length(raw: &str, location: &SourceLocation, source: &str) -> Result<u32> {
    let value = raw.parse::<u64>().map_err(|_| {
        parse_error(
            source,
            location.clone(),
            "array length must be a positive integer",
        )
    })?;
    if value == 0 || value > u32::MAX as u64 {
        return Err(parse_error(
            source,
            location.clone(),
            "array length must be between 1 and 4294967295",
        ));
    }
    Ok(value as u32)
}

fn is_likely_type_name(value: &str) -> bool {
    PrimitiveType::parse(value).is_some()
        || value
            .chars()
            .next()
            .is_some_and(|character| character.is_ascii_uppercase())
}

// This intentionally uses the union of the Rust 2024 and C++20 keyword sets,
// including contextual/reserved spellings, because every generated identifier
// must be usable by both backends.
fn is_reserved_identifier(value: &str) -> bool {
    matches!(
        value,
        // Rust 2024 strict, edition, and reserved keywords.
        "as"
            | "break"
            | "const"
            | "continue"
            | "crate"
            | "else"
            | "enum"
            | "extern"
            | "false"
            | "fn"
            | "for"
            | "if"
            | "impl"
            | "in"
            | "let"
            | "loop"
            | "match"
            | "mod"
            | "move"
            | "mut"
            | "pub"
            | "ref"
            | "return"
            | "self"
            | "Self"
            | "static"
            | "struct"
            | "super"
            | "trait"
            | "true"
            | "type"
            | "unsafe"
            | "use"
            | "where"
            | "while"
            | "async"
            | "await"
            | "dyn"
            | "abstract"
            | "become"
            | "box"
            | "do"
            | "final"
            | "macro"
            | "override"
            | "priv"
            | "typeof"
            | "unsized"
            | "virtual"
            | "yield"
            | "try"
            | "gen"
            // C++20 keywords and alternative operator spellings.
            | "alignas"
            | "alignof"
            | "and"
            | "and_eq"
            | "asm"
            | "atomic_cancel"
            | "atomic_commit"
            | "atomic_noexcept"
            | "auto"
            | "bitand"
            | "bitor"
            | "bool"
            | "case"
            | "catch"
            | "char"
            | "char8_t"
            | "char16_t"
            | "char32_t"
            | "class"
            | "compl"
            | "concept"
            | "consteval"
            | "constinit"
            | "constexpr"
            | "const_cast"
            | "co_await"
            | "co_return"
            | "co_yield"
            | "decltype"
            | "default"
            | "delete"
            | "double"
            | "dynamic_cast"
            | "explicit"
            | "export"
            | "float"
            | "friend"
            | "goto"
            | "inline"
            | "int"
            | "long"
            | "namespace"
            | "new"
            | "noexcept"
            | "not"
            | "not_eq"
            | "nullptr"
            | "operator"
            | "or"
            | "or_eq"
            | "private"
            | "protected"
            | "public"
            | "register"
            | "reinterpret_cast"
            | "requires"
            | "short"
            | "signed"
            | "sizeof"
            | "static_assert"
            | "static_cast"
            | "switch"
            | "template"
            | "this"
            | "thread_local"
            | "throw"
            | "typedef"
            | "typeid"
            | "typename"
            | "union"
            | "unsigned"
            | "using"
            | "void"
            | "volatile"
            | "wchar_t"
            | "xor"
            | "xor_eq"
            // C++20 contextual module names and conservative future words.
            | "import"
            | "module"
            | "reflexpr"
    )
}

fn validate_identifier(
    value: &str,
    what: &str,
    location: &SourceLocation,
    resolver: &Resolver<'_>,
) -> Result<()> {
    if is_reserved_identifier(value) {
        return Err(resolver.semantic_at(
            location,
            format!("{what} {value:?} is reserved by Rust 2024/C++20"),
        ));
    }
    Ok(())
}

fn validate_field_name(field: &AstField, resolver: &Resolver<'_>) -> Result<()> {
    validate_identifier(&field.name, "field name", &field.location, resolver)?;
    if field.name == "IOX2_TYPE_NAME" {
        return Err(resolver.semantic_at(
            &field.location,
            "field name IOX2_TYPE_NAME is reserved by the generated C++ type metadata",
        ));
    }
    Ok(())
}

fn register_generated_name(
    generated: &mut BTreeMap<String, (String, SourceLocation)>,
    name: &str,
    origin: String,
    location: &SourceLocation,
    resolver: &Resolver<'_>,
) -> Result<()> {
    if let Some((previous_origin, previous_location)) = generated.get(name) {
        return Err(resolver.semantic_at(
            location,
            format!(
                "generated top-level name {name:?} for {origin} collides with {previous_origin} at {previous_location}"
            ),
        ));
    }
    generated.insert(name.to_owned(), (origin, location.clone()));
    Ok(())
}

fn token_description(kind: &TokenKind) -> String {
    match kind {
        TokenKind::Ident(value) => format!("identifier {value:?}"),
        TokenKind::Integer(value) => format!("integer {value:?}"),
        TokenKind::Symbol(value) => format!("symbol {value:?}"),
        TokenKind::Eof => "end of file".to_owned(),
    }
}

/// Parses one in-memory schema.
pub fn parse(source: &str, filename: impl Into<String>) -> Result<AstSchema> {
    let filename = filename.into();
    Parser::new(lex(source, &filename)?, &filename, source).parse()
}

/// Alias for [`parse`], useful to callers that prefer an explicit name.
pub fn parse_str(source: &str, filename: impl Into<String>) -> Result<AstSchema> {
    parse(source, filename)
}

/// Reads and parses a schema file.
pub fn parse_file(path: impl AsRef<Path>) -> Result<AstSchema> {
    let path = path.as_ref();
    let source = fs::read_to_string(path).map_err(|source| Error::Io {
        path: path.to_owned(),
        source,
    })?;
    parse(&source, path.to_string_lossy().into_owned())
}

/// Names of fixed-width scalar primitives accepted by the ABI.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum PrimitiveType {
    U8,
    U16,
    U32,
    U64,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
}

impl PrimitiveType {
    pub fn parse(name: &str) -> Option<Self> {
        Some(match name {
            "u8" => Self::U8,
            "u16" => Self::U16,
            "u32" => Self::U32,
            "u64" => Self::U64,
            "i8" => Self::I8,
            "i16" => Self::I16,
            "i32" => Self::I32,
            "i64" => Self::I64,
            "f32" => Self::F32,
            "f64" => Self::F64,
            _ => return None,
        })
    }

    pub const fn as_str(self) -> &'static str {
        match self {
            Self::U8 => "u8",
            Self::U16 => "u16",
            Self::U32 => "u32",
            Self::U64 => "u64",
            Self::I8 => "i8",
            Self::I16 => "i16",
            Self::I32 => "i32",
            Self::I64 => "i64",
            Self::F32 => "f32",
            Self::F64 => "f64",
        }
    }

    pub const fn size(self) -> u32 {
        match self {
            Self::U8 | Self::I8 => 1,
            Self::U16 | Self::I16 => 2,
            Self::U32 | Self::I32 | Self::F32 => 4,
            Self::U64 | Self::I64 | Self::F64 => 8,
        }
    }

    pub const fn align(self) -> u32 {
        self.size()
    }

    pub const fn is_integer(self) -> bool {
        !matches!(self, Self::F32 | Self::F64)
    }
}

/// A checked reference to a type, including fixed arrays.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum ResolvedTypeRef {
    Primitive(PrimitiveType),
    Named(String),
    Array {
        element: Box<ResolvedTypeRef>,
        length: u32,
    },
}

impl ResolvedTypeRef {
    pub fn spelling(&self) -> String {
        match self {
            Self::Primitive(value) => value.as_str().to_owned(),
            Self::Named(value) => value.clone(),
            Self::Array { element, length } => format!("{}[{}]", element.spelling(), length),
        }
    }
}

/// One field in a resolved or canonical layout.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Serialize)]
pub struct FieldLayout {
    pub name: String,
    pub ty: String,
    pub offset: u32,
    pub size: u32,
    pub align: u32,
    pub array_length: Option<u32>,
}

/// Natural C layout of one fixed type.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Serialize)]
pub struct TypeLayout {
    pub size: u32,
    pub align: u32,
    pub fields: Vec<FieldLayout>,
}

/// A resolved primitive/newtype/struct declaration.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ResolvedTypeKind {
    Primitive(PrimitiveType),
    Newtype {
        underlying: PrimitiveType,
        constants: Vec<AstConstant>,
    },
    Struct {
        fields: Vec<ResolvedField>,
    },
}

/// A field with a checked type reference and natural layout metadata.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResolvedField {
    pub name: String,
    pub ty: ResolvedTypeRef,
    pub offset: u32,
    pub size: u32,
    pub align: u32,
    pub array_length: Option<u32>,
}

/// A type after name resolution and semantic validation.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResolvedType {
    pub name: String,
    pub kind: ResolvedTypeKind,
    pub layout: TypeLayout,
}

/// Resolved message shape.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ResolvedMessageShape {
    Fixed {
        fields: Vec<ResolvedField>,
    },
    Slice {
        header: ResolvedTypeRef,
        payload: ResolvedTypeRef,
    },
}

/// A message after name resolution and layout computation.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResolvedMessage {
    pub name: String,
    pub shape: ResolvedMessageShape,
    pub header_layout: TypeLayout,
    pub element_layout: Option<TypeLayout>,
}

/// The compiler output before canonical serialization.  It is intentionally
/// separate from the AST but still retains rich resolved type information.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResolvedSchema {
    pub filename: String,
    pub package: String,
    pub abi_major: u16,
    pub types: Vec<ResolvedType>,
    pub messages: Vec<ResolvedMessage>,
}

/// A canonical field.  `type_identity` is retained separately from the
/// spelling so a transparent newtype cannot accidentally hash as its base.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Serialize)]
pub struct CanonicalField {
    pub name: String,
    pub type_identity: String,
    pub type_spelling: String,
    pub offset: u32,
    pub size: u32,
    pub align: u32,
    pub array_length: Option<u32>,
}

/// Canonical recursive description of a fixed type.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Serialize)]
pub struct CanonicalType {
    pub identity: String,
    pub kind: String,
    pub underlying: Option<String>,
    pub constants: Vec<(String, i128)>,
    pub layout: TypeLayout,
    pub fields: Vec<CanonicalField>,
}

/// Canonical message shape.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CanonicalMessageShape {
    Fixed,
    Slice,
}

/// Canonical message model used by all output backends.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Serialize)]
pub struct CanonicalMessage {
    pub package: String,
    pub name: String,
    pub abi_major: u16,
    pub shape: CanonicalMessageShape,
    pub header_type: String,
    pub header_identity: String,
    pub header_layout: TypeLayout,
    pub element_type: Option<String>,
    pub element_identity: Option<String>,
    pub element_layout: Option<TypeLayout>,
    pub fields: Vec<CanonicalField>,
    pub layout_hash: String,
    pub signature: String,
}

/// Fully canonical schema.  The source filename is deliberately omitted from
/// this model so it cannot affect generated bytes or hashes.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Serialize)]
pub struct CanonicalSchema {
    pub package: String,
    pub abi_major: u16,
    pub endianness: String,
    pub types: Vec<CanonicalType>,
    pub messages: Vec<CanonicalMessage>,
}

pub type CanonicalModel = CanonicalSchema;

/// A 128-bit truncated BLAKE3 digest.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct LayoutHash(pub [u8; 16]);

impl LayoutHash {
    pub fn from_bytes(bytes: [u8; 16]) -> Self {
        Self(bytes)
    }

    pub fn as_bytes(&self) -> &[u8; 16] {
        &self.0
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

impl ResolvedSchema {
    pub fn canonicalize(&self) -> CanonicalSchema {
        let types = self
            .types
            .iter()
            .map(|ty| canonical_type(self, ty))
            .collect::<Vec<_>>();
        let messages = self
            .messages
            .iter()
            .map(|message| canonical_message(self, message))
            .collect::<Vec<_>>();
        CanonicalSchema {
            package: self.package.clone(),
            abi_major: self.abi_major,
            endianness: TARGET_ENDIANNESS.to_owned(),
            types,
            messages,
        }
    }

    pub fn message(&self, name: &str) -> Option<&ResolvedMessage> {
        self.messages.iter().find(|message| message.name == name)
    }
}

impl CanonicalSchema {
    pub fn message(&self, name: &str) -> Option<&CanonicalMessage> {
        self.messages.iter().find(|message| message.name == name)
    }

    pub fn descriptor_json(&self, message: &CanonicalMessage) -> String {
        let descriptor = DescriptorDocument::from_message(self, message);
        serde_json::to_string_pretty(&descriptor).expect("descriptor is serializable") + "\n"
    }

    /// Returns one deterministic descriptor document for the input package.
    /// Keeping all messages in one file makes a schema input map to exactly one
    /// output artifact while retaining the complete per-message metadata.
    pub fn schema_descriptor_json(&self) -> String {
        let document = SchemaDescriptorDocument {
            idl_language_version: IDL_LANGUAGE_VERSION,
            message_descriptor_version: MESSAGE_DESCRIPTOR_VERSION,
            endianness: TARGET_ENDIANNESS,
            package: self.package.clone(),
            abi_major: self.abi_major,
            messages: self
                .messages
                .iter()
                .map(|message| DescriptorDocument::from_message(self, message))
                .collect(),
        };
        serde_json::to_string_pretty(&document).expect("descriptor is serializable") + "\n"
    }

    /// Formats this checked schema as deterministic, source-location-free
    /// canonical `.rbt` IDL.
    ///
    /// The output is produced from the canonical checked model used by the
    /// hash and generators, not from the original source spelling. Recompiling
    /// the returned IDL therefore produces the same canonical output, making
    /// this formatter suitable for `rbt-idl fmt` and embedded schema text.
    pub fn format_idl(&self) -> String {
        format_idl(self)
    }

    /// Renders a deterministic, source-location-free `.rbt` schema.
    ///
    /// This is the durable schema representation embedded in rbt MCAP files.
    /// It is produced from the same canonical model as every language binding
    /// and descriptor, so it cannot become an independent schema truth.
    /// Prefer [`Self::format_idl`] in new code; this name remains available for
    /// compatibility with existing callers.
    pub fn canonical_idl(&self) -> String {
        self.format_idl()
    }
}

/// Formats a checked canonical schema as deterministic, source-location-free
/// `.rbt` IDL.
///
/// The formatter emits the canonical spelling accepted by the current IDL
/// grammar. It is intentionally based only on [`CanonicalSchema`], so source
/// comments, whitespace, and declaration aliases cannot affect the result.
pub fn format_idl(schema: &CanonicalSchema) -> String {
    canonical_idl(schema)
}

fn canonical_idl(schema: &CanonicalSchema) -> String {
    let mut output = format!("package {};\nabi {};\n", schema.package, schema.abi_major);
    for ty in &schema.types {
        let name = ty.identity.rsplit("::").next().unwrap_or("Type");
        match ty.kind.as_str() {
            "primitive" => {
                let underlying = ty.underlying.as_deref().unwrap_or("u8");
                output.push_str(&format!("\nprimitive {name}: {underlying};\n"));
            }
            "newtype" => {
                let underlying = ty.underlying.as_deref().unwrap_or("u8");
                output.push_str(&format!("\nnewtype {name}: {underlying}"));
                if ty.constants.is_empty() {
                    output.push_str(";\n");
                } else {
                    output.push_str(" {\n");
                    for (constant, value) in &ty.constants {
                        output.push_str(&format!("    {constant} = {value};\n"));
                    }
                    output.push_str("}\n");
                }
            }
            "struct" => {
                output.push_str(&format!("\nstruct {name} {{\n"));
                append_canonical_idl_fields(&mut output, &ty.fields);
                output.push_str("}\n");
            }
            _ => {}
        }
    }
    for message in &schema.messages {
        match message.shape {
            CanonicalMessageShape::Fixed => {
                output.push_str(&format!("\nmessage {} {{\n", message.name));
                append_canonical_idl_fields(&mut output, &message.fields);
                output.push_str("}\n");
            }
            CanonicalMessageShape::Slice => {
                output.push_str(&format!(
                    "\nmessage {} = slice {{\n    header {};\n    payload {}[];\n}}\n",
                    message.name,
                    message.header_type,
                    message.element_type.as_deref().unwrap_or("u8"),
                ));
            }
        }
    }
    output
}

fn append_canonical_idl_fields(output: &mut String, fields: &[CanonicalField]) {
    for field in fields {
        output.push_str(&format!("    {}: {};\n", field.name, field.type_spelling));
    }
}

/// Resolves names, validates the schema, and computes natural C layouts.
pub fn resolve(ast: &AstSchema) -> Result<ResolvedSchema> {
    Resolver::new(ast).run()
}

/// Alias for [`resolve`].
pub fn compile(ast: &AstSchema) -> Result<CanonicalSchema> {
    Ok(resolve(ast)?.canonicalize())
}

/// Parse, resolve, and canonicalize one schema.
pub fn compile_str(source: &str, filename: impl Into<String>) -> Result<CanonicalSchema> {
    compile(&parse(source, filename)?)
}

/// Parse, resolve, and canonicalize one schema file.
pub fn compile_file(path: impl AsRef<Path>) -> Result<CanonicalSchema> {
    compile(&parse_file(path)?)
}

struct Resolver<'a> {
    ast: &'a AstSchema,
    declarations: BTreeMap<String, &'a AstDeclaration>,
}

impl<'a> Resolver<'a> {
    fn new(ast: &'a AstSchema) -> Self {
        Self {
            ast,
            declarations: BTreeMap::new(),
        }
    }

    fn run(mut self) -> Result<ResolvedSchema> {
        if self.ast.abi_major == 0 {
            return Err(self.semantic_at(
                &SourceLocation::new(&self.ast.filename, 1, 1),
                "abi major must be greater than zero",
            ));
        }
        if self.ast.package.is_empty() || self.ast.package.split('.').any(|part| part.is_empty()) {
            return Err(self.semantic_at(
                &SourceLocation::new(&self.ast.filename, 1, 1),
                "package must contain one or more non-empty identifiers",
            ));
        }
        let package_location = SourceLocation::new(&self.ast.filename, 1, 1);
        for part in self.ast.package.split('.') {
            validate_identifier(part, "package component", &package_location, &self)?;
        }
        for declaration in &self.ast.declarations {
            let name = declaration.name().to_owned();
            validate_identifier(&name, "declaration name", declaration.location(), &self)?;
            if PrimitiveType::parse(&name).is_some() {
                return Err(self.semantic_at(
                    declaration.location(),
                    format!("declaration name {name:?} is reserved for a built-in primitive"),
                ));
            }
            if self
                .declarations
                .insert(name.clone(), declaration)
                .is_some()
            {
                return Err(
                    self.semantic_at(declaration.location(), format!("duplicate type {name:?}"))
                );
            }
        }
        self.validate_member_names()?;
        self.validate_generated_names()?;
        let mut state = HashMap::new();
        for name in self.declarations.keys() {
            let Some(location) = self
                .declarations
                .get(name)
                .map(|declaration| declaration.location())
            else {
                continue;
            };
            self.validate_type(name, location, &mut state, &mut Vec::new())?;
        }
        let mut types = Vec::new();
        for declaration in self.ast.declarations.iter().filter(|value| {
            matches!(
                value,
                AstDeclaration::Primitive(_) | AstDeclaration::Struct(_)
            )
        }) {
            match declaration {
                AstDeclaration::Primitive(value) => {
                    let underlying = value.underlying.as_deref().ok_or_else(|| {
                        self.semantic_at(
                            &value.location,
                            "newtype primitive is missing underlying type",
                        )
                    })?;
                    let primitive = PrimitiveType::parse(underlying).ok_or_else(|| {
                        self.semantic_at(
                            &value.location,
                            format!("unsupported primitive underlying type {underlying:?}"),
                        )
                    })?;
                    if !primitive.is_integer() {
                        return Err(self.semantic_at(
                            &value.location,
                            "primitive newtypes must use an integer underlying type",
                        ));
                    }
                    validate_constants(value, primitive, &self)?;
                    types.push(ResolvedType {
                        name: value.name.clone(),
                        kind: ResolvedTypeKind::Newtype {
                            underlying: primitive,
                            constants: value.constants.clone(),
                        },
                        layout: TypeLayout {
                            size: primitive.size(),
                            align: primitive.align(),
                            fields: Vec::new(),
                        },
                    });
                }
                AstDeclaration::Struct(value) => {
                    if value.fields.is_empty() {
                        return Err(self.semantic_at(
                            &value.location,
                            "struct declarations must contain at least one field",
                        ));
                    }
                    let fields = self.resolve_fields(&value.fields, &value.location, false)?;
                    let layout = layout_for_fields(&fields, &self.ast.source, &value.location)?;
                    let fields = fields_with_offsets(fields, &layout);
                    types.push(ResolvedType {
                        name: value.name.clone(),
                        kind: ResolvedTypeKind::Struct { fields },
                        layout,
                    });
                }
                _ => {}
            }
        }
        let mut messages = Vec::new();
        let mut slice_bindings = BTreeMap::<(String, String), (String, SourceLocation)>::new();
        for declaration in self
            .ast
            .declarations
            .iter()
            .filter_map(|value| match value {
                AstDeclaration::Message(message) => Some(message),
                _ => None,
            })
        {
            let message = match &declaration.shape {
                AstMessageShape::Fixed { fields } => {
                    if fields.is_empty() {
                        return Err(self.semantic_at(
                            &declaration.location,
                            "fixed message declarations must contain at least one field",
                        ));
                    }
                    let resolved_fields =
                        self.resolve_fields(fields, &declaration.location, false)?;
                    let layout = layout_for_fields(
                        &resolved_fields,
                        &self.ast.source,
                        &declaration.location,
                    )?;
                    let resolved_fields = fields_with_offsets(resolved_fields, &layout);
                    ResolvedMessage {
                        name: declaration.name.clone(),
                        shape: ResolvedMessageShape::Fixed {
                            fields: resolved_fields,
                        },
                        header_layout: layout,
                        element_layout: None,
                    }
                }
                AstMessageShape::Slice { header, payload } => {
                    if contains_nested_array(header) {
                        return Err(self.semantic_at(
                            &declaration.location,
                            "nested fixed arrays are not supported in ABI v1",
                        ));
                    }
                    if contains_dynamic(header) {
                        return Err(self.semantic_at(
                            &declaration.location,
                            "slice header must be a fixed struct type",
                        ));
                    }
                    let payload = match payload {
                        AstType::Dynamic(element)
                            if !contains_dynamic(element) && !contains_array(element) =>
                        {
                            element.as_ref()
                        }
                        AstType::Dynamic(element) if contains_nested_array(element) => {
                            return Err(self.semantic_at(
                                &declaration.location,
                                "nested fixed arrays are not supported in ABI v1",
                            ));
                        }
                        AstType::Dynamic(element) if contains_array(element) => {
                            return Err(self.semantic_at(
                                &declaration.location,
                                "slice payload element type cannot be an array",
                            ));
                        }
                        _ => {
                            return Err(self.semantic_at(
                                &declaration.location,
                                "slice payload must have the form T[] with one fixed element type",
                            ));
                        }
                    };
                    let header_ref = self.resolve_ref(header, &declaration.location)?;
                    let payload_ref = self.resolve_ref(payload, &declaration.location)?;
                    let header_layout = self.layout_of_ref(&header_ref, &declaration.location)?;
                    let element_layout = self.layout_of_ref(&payload_ref, &declaration.location)?;
                    if !matches!(header_ref, ResolvedTypeRef::Named(_)) {
                        return Err(self.semantic_at(
                            &declaration.location,
                            "slice header must name a struct type",
                        ));
                    }
                    let ResolvedTypeRef::Named(header_name) = &header_ref else {
                        return Err(self.semantic_at(
                            &declaration.location,
                            "slice header must name a struct type",
                        ));
                    };
                    if !matches!(
                        self.declarations.get(header_name),
                        Some(AstDeclaration::Struct(_))
                    ) {
                        return Err(self.semantic_at(
                            &declaration.location,
                            "slice header must name a struct type",
                        ));
                    }
                    let binding_key = (header_ref.spelling(), payload_ref.spelling());
                    if let Some((previous_message, previous_location)) =
                        slice_bindings.get(&binding_key)
                    {
                        return Err(self.semantic_at(
                            &declaration.location,
                            format!(
                                "slice message {:?} has a C++ binding collision with {:?}: header {}, element {}; previous declaration at {}",
                                declaration.name,
                                previous_message,
                                binding_key.0,
                                binding_key.1,
                                previous_location,
                            ),
                        ));
                    }
                    slice_bindings.insert(
                        binding_key,
                        (declaration.name.clone(), declaration.location.clone()),
                    );
                    ResolvedMessage {
                        name: declaration.name.clone(),
                        shape: ResolvedMessageShape::Slice {
                            header: header_ref,
                            payload: payload_ref,
                        },
                        header_layout,
                        element_layout: Some(element_layout),
                    }
                }
            };
            messages.push(message);
        }
        Ok(ResolvedSchema {
            filename: self.ast.filename.clone(),
            package: self.ast.package.clone(),
            abi_major: self.ast.abi_major,
            types,
            messages,
        })
    }

    fn semantic_at(&self, location: &SourceLocation, message: impl Into<String>) -> Error {
        Error::Semantic {
            location: location.clone(),
            message: diagnostic_message(&self.ast.source, location, message),
        }
    }

    fn validate_member_names(&self) -> Result<()> {
        for declaration in &self.ast.declarations {
            match declaration {
                AstDeclaration::Primitive(value) => {
                    for constant in &value.constants {
                        validate_identifier(
                            &constant.name,
                            "constant name",
                            &constant.location,
                            self,
                        )?;
                    }
                }
                AstDeclaration::Struct(value) => {
                    for field in &value.fields {
                        validate_field_name(field, self)?;
                    }
                }
                AstDeclaration::Message(value) => {
                    if let AstMessageShape::Fixed { fields } = &value.shape {
                        for field in fields {
                            validate_field_name(field, self)?;
                        }
                    }
                }
            }
        }
        Ok(())
    }

    fn validate_generated_names(&self) -> Result<()> {
        let mut generated = BTreeMap::<String, (String, SourceLocation)>::new();
        for declaration in &self.ast.declarations {
            if let AstDeclaration::Primitive(value) = declaration {
                let mut local_constants = BTreeSet::new();
                for constant in &value.constants {
                    // Let validate_constants report duplicate constants with
                    // its more specific diagnostic; only the first spelling
                    // participates in cross-declaration collision checking.
                    if local_constants.insert(constant.name.clone()) {
                        register_generated_name(
                            &mut generated,
                            &constant.name,
                            format!("constant {:?}", constant.name),
                            &constant.location,
                            self,
                        )?;
                    }
                }
            }
        }
        for declaration in &self.ast.declarations {
            if let AstDeclaration::Message(message) = declaration {
                let prefix = screaming_snake_case(&message.name);
                let descriptor = format!("{prefix}_DESCRIPTOR");
                register_generated_name(
                    &mut generated,
                    &descriptor,
                    format!("descriptor for message {:?}", message.name),
                    &message.location,
                    self,
                )?;
                let signature = format!("{prefix}_SIGNATURE");
                register_generated_name(
                    &mut generated,
                    &signature,
                    format!("signature for message {:?}", message.name),
                    &message.location,
                    self,
                )?;
            }
        }
        Ok(())
    }

    fn validate_type(
        &self,
        name: &str,
        reference_location: &SourceLocation,
        state: &mut HashMap<String, VisitState>,
        stack: &mut Vec<String>,
    ) -> Result<()> {
        match state.get(name) {
            Some(VisitState::Done) => return Ok(()),
            Some(VisitState::Visiting) => {
                let mut cycle = stack.join(" -> ");
                if !cycle.is_empty() {
                    cycle.push_str(" -> ");
                }
                cycle.push_str(name);
                return Err(self.semantic_at(
                    reference_location,
                    format!("recursive struct type: {cycle}"),
                ));
            }
            None => {}
        }
        let declaration = match self.declarations.get(name) {
            Some(value) => *value,
            None => return Ok(()),
        };
        state.insert(name.to_owned(), VisitState::Visiting);
        stack.push(name.to_owned());
        if let AstDeclaration::Struct(value) = declaration {
            for field in &value.fields {
                if contains_dynamic(&field.ty) {
                    return Err(self.semantic_at(
                        &field.location,
                        "dynamic slices are not allowed inside structs",
                    ));
                }
                self.validate_ref(&field.ty, &field.location, state, stack)?;
            }
        }
        stack.pop();
        state.insert(name.to_owned(), VisitState::Done);
        Ok(())
    }

    fn validate_ref(
        &self,
        ty: &AstType,
        location: &SourceLocation,
        state: &mut HashMap<String, VisitState>,
        stack: &mut Vec<String>,
    ) -> Result<()> {
        match ty {
            AstType::Named(name) => {
                if PrimitiveType::parse(name).is_none() && !self.declarations.contains_key(name) {
                    return Err(self.semantic_at(location, format!("unknown type {name:?}")));
                }
                if self.declarations.contains_key(name) {
                    self.validate_type(name, location, state, stack)?;
                }
            }
            AstType::Array { element, .. } => self.validate_ref(element, location, state, stack)?,
            AstType::Dynamic(element) => {
                self.validate_ref(element, location, state, stack)?;
            }
        }
        Ok(())
    }

    fn resolve_ref(&self, ty: &AstType, location: &SourceLocation) -> Result<ResolvedTypeRef> {
        match ty {
            AstType::Named(name) => {
                if let Some(primitive) = PrimitiveType::parse(name) {
                    Ok(ResolvedTypeRef::Primitive(primitive))
                } else if self.declarations.contains_key(name) {
                    Ok(ResolvedTypeRef::Named(name.clone()))
                } else {
                    Err(self.semantic_at(location, format!("unknown type {name:?}")))
                }
            }
            AstType::Array { element, length } => Ok(ResolvedTypeRef::Array {
                element: Box::new(self.resolve_ref(element, location)?),
                length: *length,
            }),
            AstType::Dynamic(_) => Err(self.semantic_at(
                location,
                "dynamic slice is only valid as a slice message payload",
            )),
        }
    }

    fn resolve_fields(
        &self,
        fields: &[AstField],
        declaration_location: &SourceLocation,
        allow_dynamic: bool,
    ) -> Result<Vec<ResolvedField>> {
        let mut seen = BTreeSet::new();
        let mut result = Vec::new();
        for field in fields {
            if !seen.insert(field.name.clone()) {
                return Err(
                    self.semantic_at(&field.location, format!("duplicate field {:?}", field.name))
                );
            }
            if contains_nested_array(&field.ty) {
                return Err(self.semantic_at(
                    &field.location,
                    "nested fixed arrays are not supported in ABI v1",
                ));
            }
            if !allow_dynamic && contains_dynamic(&field.ty) {
                return Err(self.semantic_at(
                    &field.location,
                    "dynamic slices are not allowed inside fixed fields",
                ));
            }
            let ty = self.resolve_ref(&field.ty, &field.location)?;
            let (size, align) = self.layout_of_ref(&ty, declaration_location)?.size_align();
            let array_length = match ty {
                ResolvedTypeRef::Array { length, .. } => Some(length),
                _ => None,
            };
            result.push(ResolvedField {
                name: field.name.clone(),
                ty,
                offset: 0,
                size,
                align,
                array_length,
            });
        }
        Ok(result)
    }

    fn layout_of_ref(&self, ty: &ResolvedTypeRef, location: &SourceLocation) -> Result<TypeLayout> {
        match ty {
            ResolvedTypeRef::Primitive(primitive) => Ok(TypeLayout {
                size: primitive.size(),
                align: primitive.align(),
                fields: Vec::new(),
            }),
            ResolvedTypeRef::Named(name) => match self.declarations.get(name) {
                Some(AstDeclaration::Primitive(value)) => {
                    let base = value
                        .underlying
                        .as_deref()
                        .and_then(PrimitiveType::parse)
                        .ok_or_else(|| {
                            self.semantic_at(
                                &value.location,
                                "primitive declaration needs a supported underlying type",
                            )
                        })?;
                    Ok(TypeLayout {
                        size: base.size(),
                        align: base.align(),
                        fields: Vec::new(),
                    })
                }
                Some(AstDeclaration::Struct(value)) => {
                    let fields = self.resolve_fields(&value.fields, &value.location, false)?;
                    layout_for_fields(&fields, &self.ast.source, &value.location)
                }
                Some(AstDeclaration::Message(_)) => Err(self.semantic_at(
                    location,
                    format!("message {name:?} cannot be used as a field type"),
                )),
                None => Err(self.semantic_at(location, format!("unknown type {name:?}"))),
            },
            ResolvedTypeRef::Array { element, length } => {
                let element_layout = self.layout_of_ref(element, location)?;
                let size = element_layout
                    .size
                    .checked_mul(*length)
                    .ok_or_else(|| self.semantic_at(location, "array size overflows u32"))?;
                Ok(TypeLayout {
                    size,
                    align: element_layout.align,
                    fields: Vec::new(),
                })
            }
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum VisitState {
    Visiting,
    Done,
}

fn contains_dynamic(ty: &AstType) -> bool {
    match ty {
        AstType::Named(_) => false,
        AstType::Array { element, .. } => contains_dynamic(element),
        AstType::Dynamic(_) => true,
    }
}

fn contains_array(ty: &AstType) -> bool {
    match ty {
        AstType::Named(_) => false,
        AstType::Array { .. } => true,
        AstType::Dynamic(element) => contains_array(element),
    }
}

fn contains_nested_array(ty: &AstType) -> bool {
    match ty {
        AstType::Named(_) => false,
        AstType::Array { element, .. } => {
            matches!(element.as_ref(), AstType::Array { .. }) || contains_nested_array(element)
        }
        AstType::Dynamic(element) => contains_nested_array(element),
    }
}

fn validate_constants(
    primitive: &AstPrimitive,
    underlying: PrimitiveType,
    resolver: &Resolver<'_>,
) -> Result<()> {
    let mut names = BTreeSet::new();
    for constant in &primitive.constants {
        if !names.insert(constant.name.clone()) {
            return Err(resolver.semantic_at(
                &constant.location,
                format!("duplicate constant {:?}", constant.name),
            ));
        }
        let valid = match underlying {
            PrimitiveType::U8 => (0..=u8::MAX as i128).contains(&constant.value),
            PrimitiveType::U16 => (0..=u16::MAX as i128).contains(&constant.value),
            PrimitiveType::U32 => (0..=u32::MAX as i128).contains(&constant.value),
            PrimitiveType::U64 => (0..=u64::MAX as i128).contains(&constant.value),
            PrimitiveType::I8 => (i8::MIN as i128..=i8::MAX as i128).contains(&constant.value),
            PrimitiveType::I16 => (i16::MIN as i128..=i16::MAX as i128).contains(&constant.value),
            PrimitiveType::I32 => (i32::MIN as i128..=i32::MAX as i128).contains(&constant.value),
            PrimitiveType::I64 => (i64::MIN as i128..=i64::MAX as i128).contains(&constant.value),
            PrimitiveType::F32 | PrimitiveType::F64 => false,
        };
        if !underlying.is_integer() || !valid {
            return Err(resolver.semantic_at(
                &constant.location,
                format!(
                    "constant value {} does not fit primitive {}",
                    constant.value,
                    underlying.as_str()
                ),
            ));
        }
    }
    Ok(())
}

fn align_up(value: u32, align: u32) -> Option<u32> {
    debug_assert!(align > 0);
    let remainder = value % align;
    if remainder == 0 {
        Some(value)
    } else {
        value.checked_add(align - remainder)
    }
}

fn layout_for_fields(
    fields: &[ResolvedField],
    source: &str,
    location: &SourceLocation,
) -> Result<TypeLayout> {
    let mut offset = 0_u32;
    let mut max_align = 1_u32;
    let mut laid_out = Vec::with_capacity(fields.len());
    for field in fields {
        let field_offset = align_up(offset, field.align).ok_or_else(|| Error::Semantic {
            location: location.clone(),
            message: diagnostic_message(source, location, "field offset overflows u32"),
        })?;
        offset = field_offset
            .checked_add(field.size)
            .ok_or_else(|| Error::Semantic {
                location: location.clone(),
                message: diagnostic_message(source, location, "struct size overflows u32"),
            })?;
        max_align = max_align.max(field.align);
        let mut field = field.clone();
        field.offset = field_offset;
        laid_out.push(field);
    }
    let size = align_up(offset, max_align).ok_or_else(|| Error::Semantic {
        location: location.clone(),
        message: diagnostic_message(source, location, "struct final padding overflows u32"),
    })?;
    Ok(TypeLayout {
        size,
        align: max_align,
        fields: laid_out
            .iter()
            .map(|field| FieldLayout {
                name: field.name.clone(),
                ty: field.ty.spelling(),
                offset: field.offset,
                size: field.size,
                align: field.align,
                array_length: field.array_length,
            })
            .collect(),
    })
}

fn fields_with_offsets(mut fields: Vec<ResolvedField>, layout: &TypeLayout) -> Vec<ResolvedField> {
    for (field, laid_out) in fields.iter_mut().zip(&layout.fields) {
        field.offset = laid_out.offset;
    }
    fields
}

impl TypeLayout {
    fn size_align(&self) -> (u32, u32) {
        (self.size, self.align)
    }
}

fn type_identity(schema: &ResolvedSchema, ty: &ResolvedTypeRef) -> String {
    match ty {
        ResolvedTypeRef::Primitive(value) => format!("primitive:{}", value.as_str()),
        ResolvedTypeRef::Named(name) => {
            let kind = schema
                .types
                .iter()
                .find(|value| value.name == *name)
                .map(|value| match value.kind {
                    ResolvedTypeKind::Newtype { .. } => "newtype",
                    ResolvedTypeKind::Primitive(_) => "primitive",
                    ResolvedTypeKind::Struct { .. } => "struct",
                })
                .unwrap_or("unknown");
            format!("{kind}:{}::{}", schema.package, name)
        }
        ResolvedTypeRef::Array { element, length } => {
            format!("array:{}:{}", length, type_identity(schema, element))
        }
    }
}

fn canonical_fields(schema: &ResolvedSchema, fields: &[ResolvedField]) -> Vec<CanonicalField> {
    fields
        .iter()
        .map(|field| CanonicalField {
            name: field.name.clone(),
            type_identity: type_identity(schema, &field.ty),
            type_spelling: field.ty.spelling(),
            offset: field.offset,
            size: field.size,
            align: field.align,
            array_length: field.array_length,
        })
        .collect()
}

fn canonical_type(schema: &ResolvedSchema, ty: &ResolvedType) -> CanonicalType {
    let (kind, underlying, constants, fields) = match &ty.kind {
        ResolvedTypeKind::Primitive(value) => (
            "primitive".to_owned(),
            Some(value.as_str().to_owned()),
            Vec::new(),
            Vec::new(),
        ),
        ResolvedTypeKind::Newtype {
            underlying,
            constants,
        } => (
            "newtype".to_owned(),
            Some(underlying.as_str().to_owned()),
            constants
                .iter()
                .map(|constant| (constant.name.clone(), constant.value))
                .collect(),
            Vec::new(),
        ),
        ResolvedTypeKind::Struct { fields } => (
            "struct".to_owned(),
            None,
            Vec::new(),
            canonical_fields(schema, fields),
        ),
    };
    CanonicalType {
        identity: format!("{}::{}", schema.package, ty.name),
        kind,
        underlying,
        constants,
        layout: ty.layout.clone(),
        fields,
    }
}

fn canonical_message(schema: &ResolvedSchema, message: &ResolvedMessage) -> CanonicalMessage {
    let (shape, header_type, header_identity, element_type, element_identity, fields) =
        match &message.shape {
            ResolvedMessageShape::Fixed { fields } => (
                CanonicalMessageShape::Fixed,
                message.name.clone(),
                format!("message:{}::{}", schema.package, message.name),
                None,
                None,
                canonical_fields(schema, fields),
            ),
            ResolvedMessageShape::Slice { header, payload } => (
                CanonicalMessageShape::Slice,
                header.spelling(),
                type_identity(schema, header),
                Some(payload.spelling()),
                Some(type_identity(schema, payload)),
                Vec::new(),
            ),
        };
    let signature = message_signature(schema, message);
    let hash = layout_hash(schema, message, &signature);
    CanonicalMessage {
        package: schema.package.clone(),
        name: message.name.clone(),
        abi_major: schema.abi_major,
        shape,
        header_type,
        header_identity,
        header_layout: message.header_layout.clone(),
        element_type,
        element_identity,
        element_layout: message.element_layout.clone(),
        fields,
        layout_hash: hash.to_string(),
        signature,
    }
}

fn message_signature(schema: &ResolvedSchema, message: &ResolvedMessage) -> String {
    match &message.shape {
        ResolvedMessageShape::Fixed { fields } => format!(
            "{};padding:{}",
            signature_fields(schema, fields),
            padding_spelling(&message.header_layout, fields)
        ),
        ResolvedMessageShape::Slice { header, payload } => format!(
            "header({},padding:{});element:{}",
            signature_type_fields(schema, header),
            padding_spelling_layout(&message.header_layout),
            element_signature(schema, payload)
        ),
    }
}

fn signature_fields(schema: &ResolvedSchema, fields: &[ResolvedField]) -> String {
    signature_fields_with_separator(schema, fields, ";")
}

fn signature_fields_with_separator(
    schema: &ResolvedSchema,
    fields: &[ResolvedField],
    separator: &str,
) -> String {
    fields
        .iter()
        .map(|field| {
            format!(
                "{}:{}@{}",
                field.name,
                signature_type(schema, &field.ty),
                field.offset
            )
        })
        .collect::<Vec<_>>()
        .join(separator)
}

fn signature_type_fields(schema: &ResolvedSchema, ty: &ResolvedTypeRef) -> String {
    match ty {
        ResolvedTypeRef::Named(name) => schema
            .types
            .iter()
            .find(|value| value.name == *name)
            .and_then(|value| match &value.kind {
                ResolvedTypeKind::Struct { fields } => {
                    Some(signature_fields_with_separator(schema, fields, ","))
                }
                _ => None,
            })
            .unwrap_or_else(|| signature_type(schema, ty)),
        _ => signature_type(schema, ty),
    }
}

fn signature_type(schema: &ResolvedSchema, ty: &ResolvedTypeRef) -> String {
    match ty {
        ResolvedTypeRef::Primitive(value) => value.as_str().to_owned(),
        ResolvedTypeRef::Named(name) => {
            if let Some(value) = schema.types.iter().find(|value| value.name == *name)
                && let ResolvedTypeKind::Struct { fields } = &value.kind
            {
                return format!(
                    "{}({})",
                    name,
                    signature_fields_with_separator(schema, fields, ",")
                );
            }
            name.clone()
        }
        ResolvedTypeRef::Array { element, length } => {
            format!("[{};{}]", signature_type(schema, element), length)
        }
    }
}

fn element_signature(schema: &ResolvedSchema, ty: &ResolvedTypeRef) -> String {
    let value = signature_type(schema, ty);
    if matches!(ty, ResolvedTypeRef::Named(name) if schema.types.iter().any(|item| item.name == *name && matches!(&item.kind, ResolvedTypeKind::Struct { .. })))
    {
        value
    } else {
        format!("{value}@0")
    }
}

fn padding_spelling(layout: &TypeLayout, fields: &[ResolvedField]) -> String {
    let end = fields
        .iter()
        .map(|field| field.offset.saturating_add(field.size))
        .max()
        .unwrap_or(0);
    if layout.size == end {
        "none".to_owned()
    } else {
        layout.size.saturating_sub(end).to_string()
    }
}

fn padding_spelling_layout(layout: &TypeLayout) -> String {
    let end = layout
        .fields
        .iter()
        .map(|field| field.offset.saturating_add(field.size))
        .max()
        .unwrap_or(0);
    if layout.size == end {
        "none".to_owned()
    } else {
        layout.size.saturating_sub(end).to_string()
    }
}

fn layout_hash_description(
    schema: &ResolvedSchema,
    message: &ResolvedMessage,
    signature: &str,
) -> String {
    let shape = match message.shape {
        ResolvedMessageShape::Fixed { .. } => "fixed",
        ResolvedMessageShape::Slice { .. } => "slice",
    };
    let element_size = message
        .element_layout
        .as_ref()
        .map_or(0, |value| value.size);
    let element_align = message
        .element_layout
        .as_ref()
        .map_or(0, |value| value.align);
    // This is intentionally the Phase 3 descriptor envelope.  The generated
    // signature is canonical and includes recursive identities, offsets,
    // sizes/alignments and array lengths, so this compact text remains a
    // complete ABI identity while preserving existing golden hashes.
    format!(
        "domain={CANONICAL_DOMAIN}\nversion={MESSAGE_DESCRIPTOR_VERSION}\nendianness={TARGET_ENDIANNESS}\nshape={shape}\nnamespace={}\nname={}\nabi_major={}\nheader_size={}\nheader_align={}\nelement_size={element_size}\nelement_align={element_align}\nsignature={signature}\n",
        schema.package,
        message.name,
        schema.abi_major,
        message.header_layout.size,
        message.header_layout.align,
    )
}

fn layout_hash(schema: &ResolvedSchema, message: &ResolvedMessage, signature: &str) -> LayoutHash {
    let description = layout_hash_description(schema, message, signature);
    let digest = blake3::hash(description.as_bytes());
    let mut bytes = [0_u8; 16];
    bytes.copy_from_slice(&digest.as_bytes()[..16]);
    LayoutHash(bytes)
}

/// Direct access to the canonical hash for callers that need to compare
/// generated output without first looking up a message.
pub fn message_layout_hash(schema: &CanonicalSchema, message_name: &str) -> Option<LayoutHash> {
    schema.message(message_name).map(|message| {
        let mut bytes = [0_u8; 16];
        for (index, pair) in message
            .layout_hash
            .as_bytes()
            .as_chunks::<2>()
            .0
            .iter()
            .enumerate()
        {
            bytes[index] = (hex(pair[0]) << 4) | hex(pair[1]);
        }
        LayoutHash(bytes)
    })
}

fn hex(value: u8) -> u8 {
    match value {
        b'0'..=b'9' => value - b'0',
        b'a'..=b'f' => value - b'a' + 10,
        _ => 0,
    }
}

#[derive(Serialize)]
struct DescriptorDocument {
    idl_language_version: u16,
    message_descriptor_version: u16,
    endianness: &'static str,
    package: String,
    namespace: String,
    name: String,
    abi_major: u16,
    shape: CanonicalMessageShape,
    header_type: String,
    header_size: u32,
    header_align: u32,
    element_type: Option<String>,
    element_size: u32,
    element_align: u32,
    layout_hash: String,
    signature: String,
    header_fields: Vec<FieldLayout>,
    element_fields: Vec<FieldLayout>,
}

#[derive(Serialize)]
struct SchemaDescriptorDocument {
    idl_language_version: u16,
    message_descriptor_version: u16,
    endianness: &'static str,
    package: String,
    abi_major: u16,
    messages: Vec<DescriptorDocument>,
}

impl DescriptorDocument {
    fn from_message(schema: &CanonicalSchema, message: &CanonicalMessage) -> Self {
        Self {
            idl_language_version: IDL_LANGUAGE_VERSION,
            message_descriptor_version: MESSAGE_DESCRIPTOR_VERSION,
            endianness: TARGET_ENDIANNESS,
            package: schema.package.clone(),
            namespace: schema.package.clone(),
            name: message.name.clone(),
            abi_major: message.abi_major,
            shape: message.shape.clone(),
            header_type: message.header_type.clone(),
            header_size: message.header_layout.size,
            header_align: message.header_layout.align,
            element_type: message.element_type.clone(),
            element_size: message.element_layout.as_ref().map_or(0, |v| v.size),
            element_align: message.element_layout.as_ref().map_or(0, |v| v.align),
            layout_hash: message.layout_hash.clone(),
            signature: message.signature.clone(),
            header_fields: message.header_layout.fields.clone(),
            element_fields: message
                .element_layout
                .as_ref()
                .map_or_else(Vec::new, |value| value.fields.clone()),
        }
    }
}

/// Output directories used by [`generate`].
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OutputDirs {
    pub rust: PathBuf,
    pub cpp: PathBuf,
    pub descriptor: PathBuf,
    pub python: Option<PathBuf>,
}

impl OutputDirs {
    pub fn new(
        rust: impl Into<PathBuf>,
        cpp: impl Into<PathBuf>,
        descriptor: impl Into<PathBuf>,
    ) -> Self {
        Self {
            rust: rust.into(),
            cpp: cpp.into(),
            descriptor: descriptor.into(),
            python: None,
        }
    }

    /// Enables the optional receive-only Python metadata backend.
    pub fn with_python(mut self, python: impl Into<PathBuf>) -> Self {
        self.python = Some(python.into());
        self
    }
}

/// Paths written for one generated schema file.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct GeneratedFiles {
    pub rust: PathBuf,
    pub cpp: PathBuf,
    pub descriptor: Vec<PathBuf>,
    pub python: Option<PathBuf>,
}

/// Generates all requested backends for a canonical schema.
pub fn generate(schema: &CanonicalSchema, dirs: &OutputDirs) -> Result<GeneratedFiles> {
    fs::create_dir_all(&dirs.rust).map_err(|source| Error::Io {
        path: dirs.rust.clone(),
        source,
    })?;
    fs::create_dir_all(&dirs.cpp).map_err(|source| Error::Io {
        path: dirs.cpp.clone(),
        source,
    })?;
    fs::create_dir_all(&dirs.descriptor).map_err(|source| Error::Io {
        path: dirs.descriptor.clone(),
        source,
    })?;
    if let Some(python) = &dirs.python {
        fs::create_dir_all(python).map_err(|source| Error::Io {
            path: python.clone(),
            source,
        })?;
    }
    let stem = package_stem(&schema.package);
    let rust_path = dirs.rust.join(format!("{stem}.rs"));
    let cpp_path = dirs.cpp.join("rbt").join(format!("{stem}.hpp"));
    if let Some(parent) = cpp_path.parent() {
        fs::create_dir_all(parent).map_err(|source| Error::Io {
            path: parent.to_owned(),
            source,
        })?;
    }
    write_if_changed(&rust_path, &generate_rust(schema)).map_err(|source| Error::Io {
        path: rust_path.clone(),
        source,
    })?;
    write_if_changed(&cpp_path, &generate_cpp(schema)).map_err(|source| Error::Io {
        path: cpp_path.clone(),
        source,
    })?;
    let path = dirs.descriptor.join(format!("{stem}.json"));
    write_if_changed(&path, &schema.schema_descriptor_json()).map_err(|source| Error::Io {
        path: path.clone(),
        source,
    })?;
    let descriptor_paths = vec![path];
    let python_path = if let Some(python) = &dirs.python {
        let path = python.join(format!("{stem}.py"));
        write_if_changed(&path, &generate_python(schema)).map_err(|source| Error::Io {
            path: path.clone(),
            source,
        })?;
        Some(path)
    } else {
        None
    };
    Ok(GeneratedFiles {
        rust: rust_path,
        cpp: cpp_path,
        descriptor: descriptor_paths,
        python: python_path,
    })
}

/// Parse, resolve, and generate one schema file.
pub fn generate_file(path: impl AsRef<Path>, dirs: &OutputDirs) -> Result<GeneratedFiles> {
    let schema = compile_file(path)?;
    generate(&schema, dirs)
}

fn write_if_changed(path: &Path, contents: &str) -> std::io::Result<()> {
    if fs::read_to_string(path).ok().as_deref() == Some(contents) {
        return Ok(());
    }
    fs::write(path, contents)
}

fn package_stem(package: &str) -> String {
    package
        .rsplit('.')
        .next()
        .map(snake_case)
        .unwrap_or_else(|| "schema".to_owned())
}

fn snake_case(value: &str) -> String {
    let mut output = String::new();
    for (index, character) in value.chars().enumerate() {
        if character.is_ascii_uppercase() {
            if index > 0 {
                output.push('_');
            }
            output.push(character.to_ascii_lowercase());
        } else if character.is_ascii_alphanumeric() || character == '_' {
            output.push(character);
        } else {
            output.push('_');
        }
    }
    if output.is_empty() {
        "schema".to_owned()
    } else {
        output
    }
}

fn screaming_snake_case(value: &str) -> String {
    let mut output = String::new();
    for (index, character) in value.chars().enumerate() {
        if character.is_ascii_uppercase() {
            if index > 0 {
                output.push('_');
            }
            output.push(character);
        } else if character.is_ascii_alphanumeric() || character == '_' {
            output.push(character.to_ascii_uppercase());
        } else {
            output.push('_');
        }
    }
    if output.is_empty() {
        "TYPE".to_owned()
    } else {
        output
    }
}

fn python_screaming_snake_case(value: &str) -> String {
    let mut output = String::new();
    let mut previous_was_lower_or_digit = false;
    for character in value.chars() {
        if character.is_ascii_uppercase() {
            if previous_was_lower_or_digit && !output.is_empty() {
                output.push('_');
            }
            output.push(character);
            previous_was_lower_or_digit = false;
        } else if character.is_ascii_lowercase() || character.is_ascii_digit() {
            output.push(character.to_ascii_uppercase());
            previous_was_lower_or_digit = true;
        } else {
            if !output.ends_with('_') {
                output.push('_');
            }
            previous_was_lower_or_digit = false;
        }
    }
    if output.is_empty() {
        "TYPE".to_owned()
    } else {
        output
    }
}

fn rust_const_string(value: &str) -> String {
    format!("{:?}", value)
}

fn generated_header() -> String {
    format!(
        "// @generated by rbt-idl; deterministic output.\n// IDL language version: {IDL_LANGUAGE_VERSION}\n// Message descriptor format version: {MESSAGE_DESCRIPTOR_VERSION}\n// DO NOT EDIT.\n\n"
    )
}

fn generated_python_header() -> String {
    format!(
        "# @generated by rbt-idl; deterministic output.\n# IDL language version: {IDL_LANGUAGE_VERSION}\n# Message descriptor format version: {MESSAGE_DESCRIPTOR_VERSION}\n# DO NOT EDIT.\n\n"
    )
}

fn python_numpy_scalar(type_spelling: &str) -> Option<&'static str> {
    match type_spelling {
        "u8" => Some("|u1"),
        "i8" => Some("|i1"),
        "u16" => Some("<u2"),
        "i16" => Some("<i2"),
        "u32" => Some("<u4"),
        "i32" => Some("<i4"),
        "u64" => Some("<u8"),
        "i64" => Some("<i8"),
        "f32" => Some("<f4"),
        "f64" => Some("<f8"),
        _ => None,
    }
}

fn python_tuple(values: impl IntoIterator<Item = String>) -> String {
    let values = values.into_iter().collect::<Vec<_>>();
    match values.as_slice() {
        [] => "()".to_owned(),
        [value] => format!("({value},)"),
        _ => format!("({})", values.join(", ")),
    }
}

/// Emits descriptor dictionaries plus NumPy dtype specifications for slice
/// element structs. Every offset and item size comes from the canonical IDL
/// layout model; this output is metadata only and never affects layout hashes.
fn generate_python(schema: &CanonicalSchema) -> String {
    let mut output = generated_python_header();
    output.push_str("MESSAGE_DESCRIPTORS = {\n");
    for message in &schema.messages {
        let element_size = message
            .element_layout
            .as_ref()
            .map_or(0, |layout| layout.size);
        let element_align = message
            .element_layout
            .as_ref()
            .map_or(0, |layout| layout.align);
        let shape = if matches!(message.shape, CanonicalMessageShape::Fixed) {
            "fixed"
        } else {
            "slice"
        };
        output.push_str(&format!(
            "    {:?}: {{\"namespace\": {:?}, \"name\": {:?}, \"abi_major\": {}, \"shape\": {:?}, \"header_size\": {}, \"header_align\": {}, \"element_size\": {}, \"element_align\": {}, \"layout_hash\": {:?}}},\n",
            message.name,
            schema.package,
            message.name,
            message.abi_major,
            shape,
            message.header_layout.size,
            message.header_layout.align,
            element_size,
            element_align,
            message.layout_hash,
        ));
    }
    output.push_str("}\n\n");
    for message in &schema.messages {
        output.push_str(&format!(
            "{}_DESCRIPTOR = MESSAGE_DESCRIPTORS[{:?}]\n",
            python_screaming_snake_case(&message.name),
            message.name,
        ));
    }

    let mut emitted_specs = Vec::new();
    for message in &schema.messages {
        let Some(element_identity) = &message.element_identity else {
            continue;
        };
        let Some(element_identity) = element_identity.strip_prefix("struct:") else {
            continue;
        };
        let Some(ty) = schema
            .types
            .iter()
            .find(|ty| ty.identity == element_identity && ty.kind == "struct")
        else {
            continue;
        };
        let formats = ty
            .fields
            .iter()
            .map(|field| python_numpy_scalar(&field.type_spelling))
            .collect::<Option<Vec<_>>>();
        let Some(formats) = formats else {
            continue;
        };
        let type_name = ty.identity.rsplit("::").next().unwrap_or("Type");
        let constant = format!("{}_DTYPE_SPEC", python_screaming_snake_case(type_name));
        output.push('\n');
        output.push_str(&format!("{constant} = {{\n"));
        output.push_str(&format!(
            "    \"names\": {},\n",
            python_tuple(ty.fields.iter().map(|field| format!("{:?}", field.name)))
        ));
        output.push_str(&format!(
            "    \"formats\": {},\n",
            python_tuple(formats.into_iter().map(|format| format!("{format:?}")))
        ));
        output.push_str(&format!(
            "    \"offsets\": {},\n",
            python_tuple(ty.fields.iter().map(|field| field.offset.to_string()))
        ));
        output.push_str(&format!("    \"itemsize\": {},\n", ty.layout.size));
        output.push_str("}\n");
        emitted_specs.push((ty.identity.clone(), constant));
    }
    output.push_str("\nNUMPY_DTYPE_SPECS = {\n");
    for (identity, constant) in emitted_specs {
        output.push_str(&format!("    {identity:?}: {constant},\n"));
    }
    output.push_str("}\n");
    output
}

fn generate_rust(schema: &CanonicalSchema) -> String {
    let mut output = generated_header();
    let file_schema = schema.canonical_idl();
    for ty in &schema.types {
        match ty.kind.as_str() {
            "newtype" => {
                let name = ty.identity.rsplit("::").next().unwrap_or("Type");
                let underlying = ty.underlying.as_deref().unwrap_or("u8");
                output.push_str("#[repr(transparent)]\n#[derive(Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]\n");
                output.push_str(&format!("pub struct {name}(pub {underlying});\n"));
                output.push_str(&format!(
                    "// SAFETY: Generated {name} is repr(transparent), Copy, and contains only a fixed-width ABI primitive.\n"
                ));
                output.push_str(&format!("unsafe impl crate::ShmSafe for {name} {{}}\n"));
                output.push_str(&format!(
                    "// SAFETY: Generated {name} has a stable shared-memory representation with no process-local state.\n"
                ));
                output.push_str(&format!(
                    "unsafe impl iceoryx2::prelude::ZeroCopySend for {name} {{ unsafe fn type_name() -> &'static str {{ {} }} }}\n",
                    rust_const_string(&format!("{}/{}@{}", schema.package, name, schema.abi_major))
                ));
                append_rust_asserts(&mut output, name, &ty.layout);
                output.push_str(&format!("impl {name} {{\n"));
                output.push_str(&format!(
                    "    pub const TYPE_NAMESPACE: &'static str = {};\n",
                    rust_const_string(&schema.package)
                ));
                output.push_str(&format!(
                    "    pub const TYPE_NAME: &'static str = {};\n",
                    rust_const_string(name)
                ));
                output.push_str(&format!(
                    "    pub const ABI_MAJOR_VERSION: u16 = {};\n",
                    schema.abi_major
                ));
                for (constant_name, value) in &ty.constants {
                    output.push_str(&format!(
                        "    pub const {constant_name}: Self = Self({value});\n"
                    ));
                }
                output.push_str(&format!(
                    "    pub const fn new(value: {underlying}) -> Self {{ Self(value) }}\n"
                ));
                output.push_str("    pub const fn value(self) -> ");
                output.push_str(underlying);
                output.push_str(" { self.0 }\n    pub const fn get(self) -> ");
                output.push_str(underlying);
                output.push_str(" { self.value() }\n}\n");
                output.push_str(&format!(
                    "impl crate::CanonicalFieldCodec for {name} {{\n    const ENCODED_SIZE: usize = <{underlying} as crate::CanonicalFieldCodec>::ENCODED_SIZE;\n    fn encode_canonical(&self, output: &mut Vec<u8>) {{ <{underlying} as crate::CanonicalFieldCodec>::encode_canonical(&self.0, output); }}\n    fn decode_canonical(&mut self, cursor: &mut crate::CanonicalCursor<'_>) -> Result<(), crate::CanonicalDecodeError> {{ <{underlying} as crate::CanonicalFieldCodec>::decode_canonical(&mut self.0, cursor) }}\n}}\n"
                ));
                output.push('\n');
            }
            "struct" => {
                let name = ty.identity.rsplit("::").next().unwrap_or("Type");
                append_rust_record(&mut output, name, &ty.fields);
                output.push_str(&format!(
                    "// SAFETY: Generated {name} is repr(C), Copy, and contains only shared-memory-safe fields.\n"
                ));
                output.push_str(&format!("unsafe impl crate::ShmSafe for {name} {{}}\n"));
                output.push_str(&format!(
                    "// SAFETY: Generated {name} has a stable shared-memory representation with no process-local state.\n"
                ));
                output.push_str(&format!(
                    "unsafe impl iceoryx2::prelude::ZeroCopySend for {name} {{ unsafe fn type_name() -> &'static str {{ {} }} }}\n",
                    rust_const_string(&format!("{}/{}@{}", schema.package, name, schema.abi_major))
                ));
                append_rust_asserts(&mut output, name, &ty.layout);
                append_rust_codec_impl(&mut output, name, &ty.fields);
                output.push('\n');
            }
            _ => {}
        }
    }
    for message in &schema.messages {
        let name = &message.name;
        if matches!(message.shape, CanonicalMessageShape::Fixed) {
            append_rust_record(&mut output, name, &message.fields);
            output.push_str(&format!(
                "// SAFETY: Generated {name} is repr(C), Copy, and contains only shared-memory-safe fields.\n"
            ));
            output.push_str(&format!("unsafe impl crate::ShmSafe for {name} {{}}\n"));
            append_rust_asserts(&mut output, name, &message.header_layout);
            append_rust_codec_impl(&mut output, name, &message.fields);
            append_rust_descriptor(&mut output, schema, message, name, true, &file_schema);
        } else {
            output.push_str("#[repr(C)]\n#[derive(Clone, Copy, Debug, Default)]\n");
            output.push_str(&format!("pub struct {name};\n"));
            output.push_str(&format!(
                "// SAFETY: Generated {name} is a repr(C), Copy marker with no process-local state.\n"
            ));
            output.push_str(&format!("unsafe impl crate::ShmSafe for {name} {{}}\n"));
            append_rust_descriptor(&mut output, schema, message, name, false, &file_schema);
        }
        output.push('\n');
    }
    output
}

fn append_rust_record(output: &mut String, name: &str, fields: &[CanonicalField]) {
    output.push_str("#[repr(C)]\n");
    if fields.iter().any(rust_field_needs_manual_array_support) {
        output.push_str("#[derive(Clone, Copy, Debug)]\n");
    } else {
        output.push_str("#[derive(Clone, Copy, Debug, Default)]\n");
    }
    output.push_str(&format!("pub struct {name} {{\n"));
    for field in fields {
        output.push_str(&format!(
            "    pub {}: {},\n",
            rust_field_name(&field.name),
            field_rust_type(field)
        ));
    }
    output.push_str("}\n");
    if fields.iter().any(rust_field_needs_manual_array_support) {
        output.push_str(&format!(
            "impl Default for {name} {{\n    fn default() -> Self {{\n        Self {{\n"
        ));
        for field in fields {
            let default = if rust_field_needs_manual_array_support(field) {
                format!("[Default::default(); {}]", field.array_length.unwrap())
            } else {
                "Default::default()".to_owned()
            };
            output.push_str(&format!(
                "            {}: {default},\n",
                rust_field_name(&field.name)
            ));
        }
        output.push_str("        }\n    }\n}\n");
    }
}

fn rust_field_needs_manual_array_support(field: &CanonicalField) -> bool {
    field.array_length.is_some_and(|length| length > 32)
}

fn append_rust_codec_impl(output: &mut String, name: &str, fields: &[CanonicalField]) {
    let (first, remaining) = fields
        .split_first()
        .expect("validated structs and fixed messages always have at least one field");
    output.push_str(&format!(
        "impl crate::CanonicalFieldCodec for {name} {{\n    const ENCODED_SIZE: usize = {}",
        rust_field_encoded_size(first)
    ));
    for field in remaining {
        output.push_str(&format!(" + {}", rust_field_encoded_size(field)));
    }
    output.push_str(";\n    fn encode_canonical(&self, output: &mut Vec<u8>) {\n");
    for field in fields {
        if rust_field_needs_manual_array_support(field) {
            output.push_str(&format!(
                "        for value in &self.{} {{\n            crate::CanonicalFieldCodec::encode_canonical(value, output);\n        }}\n",
                rust_field_name(&field.name)
            ));
        } else {
            output.push_str(&format!(
                "        crate::CanonicalFieldCodec::encode_canonical(&self.{}, output);\n",
                rust_field_name(&field.name)
            ));
        }
    }
    output.push_str(
        "    }\n    fn decode_canonical(&mut self, cursor: &mut crate::CanonicalCursor<'_>) -> Result<(), crate::CanonicalDecodeError> {\n",
    );
    for field in fields {
        if rust_field_needs_manual_array_support(field) {
            output.push_str(&format!(
                "        for value in &mut self.{} {{\n            crate::CanonicalFieldCodec::decode_canonical(value, cursor)?;\n        }}\n",
                rust_field_name(&field.name)
            ));
        } else {
            output.push_str(&format!(
                "        crate::CanonicalFieldCodec::decode_canonical(&mut self.{}, cursor)?;\n",
                rust_field_name(&field.name)
            ));
        }
    }
    output.push_str("        Ok(())\n    }\n}\n");
}

fn rust_field_encoded_size(field: &CanonicalField) -> String {
    if rust_field_needs_manual_array_support(field) {
        let length = field.array_length.unwrap();
        let element_type = strip_array_suffix(&field.type_spelling, length);
        format!(
            "<{} as crate::CanonicalFieldCodec>::ENCODED_SIZE * {length}",
            rust_named_spelling(&element_type)
        )
    } else {
        format!(
            "<{} as crate::CanonicalFieldCodec>::ENCODED_SIZE",
            field_rust_type(field)
        )
    }
}

fn rust_field_name(name: &str) -> String {
    match name {
        "type" | "ref" | "self" | "Self" | "crate" | "super" | "mod" | "struct" | "enum" | "fn"
        | "const" | "static" | "move" | "match" | "loop" | "impl" | "trait" | "where" | "use"
        | "pub" | "in" | "as" | "async" | "await" | "dyn" => format!("r#{name}"),
        _ => name.to_owned(),
    }
}

fn field_rust_type(field: &CanonicalField) -> String {
    if let Some(length) = field.array_length {
        let base = strip_array_suffix(&field.type_spelling, length);
        format!("[{}; {}]", rust_named_spelling(&base), length)
    } else {
        rust_named_spelling(&field.type_spelling)
    }
}

fn rust_named_spelling(value: &str) -> String {
    match value {
        "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64" | "f32" | "f64" => {
            value.to_owned()
        }
        _ => value.to_owned(),
    }
}

fn append_rust_asserts(output: &mut String, name: &str, layout: &TypeLayout) {
    output.push_str("const _: () = {\n");
    output.push_str(&format!(
        "    assert!(::core::mem::size_of::<{name}>() == {});\n",
        layout.size
    ));
    output.push_str(&format!(
        "    assert!(::core::mem::align_of::<{name}>() == {});\n",
        layout.align
    ));
    for field in &layout.fields {
        output.push_str(&format!(
            "    assert!(::core::mem::offset_of!({name}, {}) == {});\n",
            rust_field_name(&field.name),
            field.offset
        ));
    }
    output.push_str("};\n");
}

fn append_rust_descriptor(
    output: &mut String,
    schema: &CanonicalSchema,
    message: &CanonicalMessage,
    name: &str,
    fixed: bool,
    file_schema: &str,
) {
    let const_prefix = screaming_snake_case(name);
    let const_name = format!("{const_prefix}_DESCRIPTOR");
    let hash = hash_bytes_literal(&message.layout_hash);
    let element_size = message
        .element_layout
        .as_ref()
        .map_or(0, |layout| layout.size);
    let element_align = message
        .element_layout
        .as_ref()
        .map_or(0, |layout| layout.align);
    output.push_str(&format!(
        "pub const {const_name}: crate::StaticMessageTypeDescriptor = crate::StaticMessageTypeDescriptor {{ namespace: {}, name: {}, abi_major: {}, shape: crate::MessageShape::{}, header_size: {}, header_align: {}, element_size: {}, element_align: {}, layout_hash: crate::LayoutHash::new({hash}) }};\n",
        rust_const_string(&schema.package),
        rust_const_string(name),
        message.abi_major,
        if fixed { "Fixed" } else { "Slice" },
        message.header_layout.size,
        message.header_layout.align,
        element_size,
        element_align,
    ));
    output.push_str(&format!(
        "pub const {}_SIGNATURE: &str = {};\n",
        const_prefix,
        rust_const_string(&message.signature)
    ));
    output.push_str(&format!(
        "pub const {}_FILE_SCHEMA: &str = {};\n",
        const_prefix,
        rust_const_string(file_schema)
    ));
    output.push_str(&format!(
        "impl {name} {{ pub const TYPE_NAMESPACE: &'static str = {}; pub const TYPE_NAME: &'static str = {}; pub const ABI_MAJOR_VERSION: u16 = {}; pub const LAYOUT_SIGNATURE: &'static str = {}_SIGNATURE; pub fn descriptor() -> crate::MessageTypeDescriptor {{ <Self as crate::{}MessageAbi>::descriptor() }} }}\n",
        rust_const_string(&schema.package),
        rust_const_string(name),
        message.abi_major,
        const_prefix,
        if fixed { "Fixed" } else { "Slice" },
    ));
    output.push_str(&format!(
        "// SAFETY: Generated {name} has a stable shared-memory representation with no process-local state.\n"
    ));
    output.push_str(&format!(
        "unsafe impl iceoryx2::prelude::ZeroCopySend for {name} {{ unsafe fn type_name() -> &'static str {{ {} }} }}\n",
        rust_const_string(&format!("{}/{}@{}", schema.package, name, message.abi_major))
    ));
    if fixed {
        output.push_str(&format!(
            "impl crate::FixedMessageAbi for {name} {{ const NAMESPACE: &'static str = {}; const NAME: &'static str = {}; const ABI_MAJOR: u16 = {}; const SIGNATURE: &'static str = {}_SIGNATURE; const DESCRIPTOR: Option<crate::StaticMessageTypeDescriptor> = Some({const_name}); }}\n",
            rust_const_string(&schema.package), rust_const_string(name), message.abi_major, const_prefix
        ));
        output.push_str(&format!(
            "impl crate::CanonicalFixedMessage for {name} {{ const FILE_SCHEMA: &'static str = {const_prefix}_FILE_SCHEMA; }}\n"
        ));
    } else {
        let header = message.header_type.clone();
        let payload = message
            .element_type
            .clone()
            .unwrap_or_else(|| "u8".to_owned());
        output.push_str(&format!(
            "impl crate::SliceMessageAbi for {name} {{ type Header = {}; type Element = {}; const NAMESPACE: &'static str = {}; const NAME: &'static str = {}; const ABI_MAJOR: u16 = {}; const SIGNATURE: &'static str = {}_SIGNATURE; const DESCRIPTOR: Option<crate::StaticMessageTypeDescriptor> = Some({const_name}); }}\n",
            rust_named_spelling(&header), rust_named_spelling(&payload), rust_const_string(&schema.package), rust_const_string(name), message.abi_major, const_prefix
        ));
        output.push_str(&format!(
            "impl crate::CanonicalSliceMessage for {name} {{ const FILE_SCHEMA: &'static str = {const_prefix}_FILE_SCHEMA; }}\n"
        ));
    }
}

fn hash_bytes_literal(hash: &str) -> String {
    let mut bytes = Vec::new();
    for pair in hash.as_bytes().as_chunks::<2>().0 {
        bytes.push(format!("0x{}{}", pair[0] as char, pair[1] as char));
    }
    format!("[{}]", bytes.join(", "))
}

fn generate_cpp(schema: &CanonicalSchema) -> String {
    let mut output = generated_header();
    output.push_str("#pragma once\n\n#include <bit>\n#include <cstddef>\n#include <cstdint>\n#include <type_traits>\n#include <rbt/types.hpp>\n#include <rbt/version.hpp>\n\n");
    for part in schema.package.split('.') {
        output.push_str(&format!("namespace {part} {{\n"));
    }
    output.push('\n');
    output.push_str("static_assert(std::endian::native == std::endian::little, \"rbt ABI requires little endian\");\n");
    output.push_str("static_assert(sizeof(float) == 4);\nstatic_assert(sizeof(double) == 8);\n");
    for ty in &schema.types {
        let name = ty.identity.rsplit("::").next().unwrap_or("Type");
        match ty.kind.as_str() {
            "newtype" => {
                let underlying = cpp_scalar(ty.underlying.as_deref().unwrap_or("u8"));
                output.push_str(&format!("struct {name} {{ {underlying} value; inline static constexpr char IOX2_TYPE_NAME[] = \"{}/{}@{}\"; }};\n", schema.package, name, schema.abi_major));
                output.push_str(&format!("static_assert(std::is_standard_layout_v<{name}>);\nstatic_assert(std::is_trivially_copyable_v<{name}>);\nstatic_assert(sizeof({name}) == {});\nstatic_assert(alignof({name}) == {});\n", ty.layout.size, ty.layout.align));
                for (constant, value) in &ty.constants {
                    output.push_str(&format!("inline constexpr {name} {constant}{{static_cast<{underlying}>({value})}};\n"));
                }
            }
            "struct" => {
                output.push_str(&format!("struct {name} {{\n"));
                for field in &ty.fields {
                    output.push_str(&format!("    {};\n", cpp_field_decl(field)));
                }
                output.push_str(&format!(
                    "    inline static constexpr char IOX2_TYPE_NAME[] = \"{}/{}@{}\";\n",
                    schema.package, name, schema.abi_major
                ));
                output.push_str("};\n");
                append_cpp_asserts(&mut output, name, &ty.layout);
            }
            _ => {}
        }
        output.push('\n');
    }
    for message in &schema.messages {
        if matches!(message.shape, CanonicalMessageShape::Fixed) {
            output.push_str(&format!("struct {} {{\n", message.name));
            for field in &message.fields {
                output.push_str(&format!("    {};\n", cpp_field_decl(field)));
            }
            output.push_str(&format!(
                "    inline static constexpr char IOX2_TYPE_NAME[] = \"{}/{}@{}\";\n",
                schema.package, message.name, schema.abi_major
            ));
            output.push_str("};\n");
            append_cpp_asserts(&mut output, &message.name, &message.header_layout);
        } else {
            output.push_str(&format!(
                "struct {} {{ inline static constexpr char IOX2_TYPE_NAME[] = \"{}/{}@{}\"; }};\n",
                message.name, schema.package, message.name, schema.abi_major
            ));
            output.push_str(&format!(
                "static_assert(std::is_standard_layout_v<{}>);\n",
                message.name
            ));
            output.push_str(&format!(
                "static_assert(std::is_trivially_copyable_v<{}>);\n",
                message.name
            ));
        }
        let hash = cpp_hash_literal(&message.layout_hash);
        let element_size = message
            .element_layout
            .as_ref()
            .map_or(0, |layout| layout.size);
        let element_align = message
            .element_layout
            .as_ref()
            .map_or(0, |layout| layout.align);
        let const_prefix = screaming_snake_case(&message.name);
        let signature_name = format!("{const_prefix}_SIGNATURE");
        output.push_str(&format!(
            "inline constexpr char {signature_name}[] = {};\n",
            cpp_string_literal(&message.signature)
        ));
        output.push_str(&format!("inline constexpr rbt::MessageTypeDescriptor {const_prefix}_DESCRIPTOR{{\"{}\", \"{}\", {}, rbt::MessageShape::{}, {}, {}, {}, {}, {hash}, {signature_name}}};\n", schema.package, message.name, message.abi_major, if matches!(message.shape, CanonicalMessageShape::Fixed) { "Fixed" } else { "Slice" }, message.header_layout.size, message.header_layout.align, element_size, element_align));
        output.push('\n');
    }
    for _ in schema.package.split('.') {
        output.push_str("} // namespace\n");
    }
    output.push_str("\nnamespace rbt {\n");
    for message in &schema.messages {
        let message_type = cpp_qualified_type(&schema.package, &message.name);
        let descriptor = format!(
            "{}{}_DESCRIPTOR",
            cpp_qualified_namespace(&schema.package),
            screaming_snake_case(&message.name)
        );
        match message.shape {
            CanonicalMessageShape::Fixed => output.push_str(&format!(
                "template <>\nstruct FixedMessageBinding<{message_type}> {{\n    static constexpr const MessageTypeDescriptor& descriptor() noexcept {{\n        return {descriptor};\n    }}\n}};\n\n"
            )),
            CanonicalMessageShape::Slice => {
                let header_type = cpp_qualified_type(&schema.package, &message.header_type);
                let element_type = cpp_scalar_or_named(
                    message
                        .element_type
                        .as_deref()
                        .unwrap_or("u8"),
                );
                let element_type = if element_type.starts_with("std::") {
                    element_type
                } else {
                    cpp_qualified_type(&schema.package, &element_type)
                };
                output.push_str(&format!(
                    "template <>\nstruct SliceMessageBinding<{header_type}, {element_type}> {{\n    static constexpr const MessageTypeDescriptor& descriptor() noexcept {{\n        return {descriptor};\n    }}\n}};\n\n"
                ));
            }
        }
    }
    output.push_str("} // namespace rbt\n");
    output
}

fn cpp_qualified_namespace(package: &str) -> String {
    format!("::{}::", package.replace('.', "::"))
}

fn cpp_qualified_type(package: &str, name: &str) -> String {
    format!("{}{name}", cpp_qualified_namespace(package))
}

fn cpp_scalar(value: &str) -> &'static str {
    match value {
        "u8" => "std::uint8_t",
        "u16" => "std::uint16_t",
        "u32" => "std::uint32_t",
        "u64" => "std::uint64_t",
        "i8" => "std::int8_t",
        "i16" => "std::int16_t",
        "i32" => "std::int32_t",
        "i64" => "std::int64_t",
        "f32" => "float",
        "f64" => "double",
        _ => "std::uint8_t",
    }
}

fn cpp_field_decl(field: &CanonicalField) -> String {
    let ty = field.array_length.map_or_else(
        || field.type_spelling.clone(),
        |length| strip_array_suffix(&field.type_spelling, length),
    );
    let ty = cpp_scalar_or_named(&ty);
    if let Some(length) = field.array_length {
        format!("{ty} {}[{length}]", field.name)
    } else {
        format!("{ty} {}", field.name)
    }
}

fn strip_array_suffix(value: &str, length: u32) -> String {
    let suffix = format!("[{length}]");
    value
        .strip_suffix(&suffix)
        .map_or_else(|| value.to_owned(), ToOwned::to_owned)
}

fn cpp_scalar_or_named(value: &str) -> String {
    if let Some(base) = value.strip_suffix("]") {
        let _ = base;
    }
    match value {
        "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64" | "f32" | "f64" => {
            cpp_scalar(value).to_owned()
        }
        _ => value.to_owned(),
    }
}

fn append_cpp_asserts(output: &mut String, name: &str, layout: &TypeLayout) {
    output.push_str(&format!("static_assert(std::is_standard_layout_v<{name}>);\nstatic_assert(std::is_trivially_copyable_v<{name}>);\nstatic_assert(sizeof({name}) == {});\nstatic_assert(alignof({name}) == {});\n", layout.size, layout.align));
    for field in &layout.fields {
        output.push_str(&format!(
            "static_assert(offsetof({name}, {}) == {});\n",
            field.name, field.offset
        ));
    }
}

fn cpp_hash_literal(value: &str) -> String {
    let bytes = value
        .as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|pair| format!("0x{}{}", pair[0] as char, pair[1] as char))
        .collect::<Vec<_>>();
    format!("{{{}}}", bytes.join(", "))
}

fn cpp_string_literal(value: &str) -> String {
    let mut output = String::with_capacity(value.len() + 2);
    output.push('"');
    for character in value.chars() {
        match character {
            '\\' => output.push_str("\\\\"),
            '"' => output.push_str("\\\""),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            '\0' => output.push_str("\\0"),
            character if character.is_ascii() && character.is_ascii_control() => {
                output.push_str(&format!("\\x{:02x}", character as u32));
            }
            character => output.push(character),
        }
    }
    output.push('"');
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    const SENSOR: &str = r#"
        // comments do not participate in the hash
        package rbt.sensor;
        abi = 1;
        primitive PixelFormat: u32 { RGB8 = 1; BGR8 = 2; }
        struct Header { sequence: u64; format: PixelFormat; }
        message Image = slice { header Header; payload u8[]; }
    "#;

    #[test]
    fn parser_and_natural_layout() {
        let schema = compile_str(SENSOR, "sensor.rbt").unwrap();
        let header = schema
            .types
            .iter()
            .find(|ty| ty.identity.ends_with("::Header"))
            .unwrap();
        assert_eq!(header.layout.size, 16);
        assert_eq!(header.layout.align, 8);
        assert_eq!(header.layout.fields[1].offset, 8);
        let image = schema.message("Image").unwrap();
        assert_eq!(image.shape, CanonicalMessageShape::Slice);
        assert_eq!(image.header_layout.size, 16);
        assert_eq!(image.element_layout.as_ref().unwrap().size, 1);
    }

    #[test]
    fn comments_and_whitespace_do_not_change_hash() {
        let one = compile_str(SENSOR, "a.rbt").unwrap();
        let two = compile_str("package rbt.sensor; abi 1; primitive PixelFormat:u32{RGB8=1;BGR8=2;} struct Header{sequence:u64;format:PixelFormat;} message Image=slice{header Header;payload u8[];}", "b.rbt").unwrap();
        assert_eq!(
            one.message("Image").unwrap().layout_hash,
            two.message("Image").unwrap().layout_hash
        );
    }

    #[test]
    fn validation_reports_bad_types_and_cycles() {
        let error = compile_str(
            "package p; abi 1; struct A { b: B; } struct B { a: A; }",
            "cycle.rbt",
        )
        .unwrap_err();
        assert!(error.to_string().contains("cycle.rbt"));
        assert!(error.to_string().contains("recursive"));
        let error =
            compile_str("package p; abi 1; struct A { b: Missing; }", "unknown.rbt").unwrap_err();
        assert!(error.to_string().contains("unknown type"));
    }

    #[test]
    fn diagnostics_include_stable_source_context() {
        let error = compile_str(
            "package p;\nabi 1;\nstruct Node {\n    next: Node;\n}\n",
            "recursive.rbt",
        )
        .unwrap_err()
        .to_string();
        assert_eq!(
            error,
            "recursive.rbt:4:5: recursive struct type: Node -> Node\n  |\n4 |     next: Node;\n  |     ^"
        );

        let error = parse("package p;\nabi 1;\nmessage M { value }\n", "parse.rbt")
            .unwrap_err()
            .to_string();
        assert_eq!(
            error,
            "parse.rbt:3:13: expected ':' or a field type after field name\n  |\n3 | message M { value }\n  |             ^"
        );
    }

    #[test]
    fn hashes_change_for_field_order_and_newtype_identity() {
        let first = compile_str("package p; abi 1; message M { a: u8; b: u16; }", "a.rbt").unwrap();
        let second =
            compile_str("package p; abi 1; message M { b: u16; a: u8; }", "b.rbt").unwrap();
        assert_ne!(
            first.message("M").unwrap().layout_hash,
            second.message("M").unwrap().layout_hash
        );
        let plain = compile_str("package p; abi 1; message M { a: u32; }", "a.rbt").unwrap();
        let named = compile_str(
            "package p; abi 1; primitive X:u32; message M { a: X; }",
            "b.rbt",
        )
        .unwrap();
        assert_ne!(
            plain.message("M").unwrap().layout_hash,
            named.message("M").unwrap().layout_hash
        );
    }

    #[test]
    fn hashes_change_for_added_field_and_type_change() {
        let base = compile_str("package p; abi 1; message M { a: u32; }", "base.rbt").unwrap();
        let added = compile_str(
            "package p; abi 1; message M { a: u32; b: u8; }",
            "added.rbt",
        )
        .unwrap();
        let changed =
            compile_str("package p; abi 1; message M { a: u64; }", "changed.rbt").unwrap();
        assert_ne!(
            base.message("M").unwrap().layout_hash,
            added.message("M").unwrap().layout_hash
        );
        assert_ne!(
            base.message("M").unwrap().layout_hash,
            changed.message("M").unwrap().layout_hash
        );
    }

    #[test]
    fn explicit_slice_syntax_does_not_reclassify_fixed_header_payload_fields() {
        let fixed = compile_str(
            "package p; abi 1; message M { header: u8; payload: u16; }",
            "fixed.rbt",
        )
        .unwrap();
        let message = fixed.message("M").unwrap();
        assert_eq!(message.shape, CanonicalMessageShape::Fixed);
        assert_eq!(
            message
                .fields
                .iter()
                .map(|field| field.name.as_str())
                .collect::<Vec<_>>(),
            ["header", "payload"]
        );

        let inline_slice = compile_str(
            "package p; abi 1; struct H { value: u8; } message M = slice { header H; payload u16[]; } message N { value: u8; }",
            "inline-slice.rbt",
        )
        .unwrap();
        assert_eq!(
            inline_slice.message("M").unwrap().shape,
            CanonicalMessageShape::Slice
        );
        assert!(inline_slice.message("N").is_some());

        let prefix_slice = compile_str(
            "package p; abi 1; struct H { value: u8; } slice M { header H; payload u16[]; }; message N { value: u8; }",
            "prefix-slice.rbt",
        )
        .unwrap();
        assert_eq!(
            prefix_slice.message("M").unwrap().shape,
            CanonicalMessageShape::Slice
        );
        assert!(prefix_slice.message("N").is_some());
    }

    #[test]
    fn rejects_v1_nested_arrays_and_array_slice_elements() {
        let nested = compile_str(
            "package p; abi 1; struct S { values: u8[2][3]; }",
            "nested-array.rbt",
        )
        .unwrap_err();
        assert!(nested.to_string().contains("nested fixed arrays"));

        let slice_element = compile_str(
            "package p; abi 1; struct H { value: u8; } message M = slice { header H; payload u8[2][]; }",
            "array-element.rbt",
        )
        .unwrap_err();
        assert!(
            slice_element
                .to_string()
                .contains("slice payload element type cannot be an array")
        );
    }

    #[test]
    fn rejects_empty_structs_and_fixed_messages() {
        let empty_struct =
            compile_str("package p; abi 1; struct Empty {}", "empty-struct.rbt").unwrap_err();
        assert!(empty_struct.to_string().contains("struct declarations"));

        let empty_message =
            compile_str("package p; abi 1; message Empty {}", "empty-message.rbt").unwrap_err();
        assert!(
            empty_message
                .to_string()
                .contains("fixed message declarations")
        );
    }

    #[test]
    fn rejects_builtin_declaration_names() {
        let error = compile_str("package p; abi 1; primitive u32;", "builtin.rbt").unwrap_err();
        assert!(
            error
                .to_string()
                .contains("reserved for a built-in primitive")
        );
    }

    #[test]
    fn rejects_cross_backend_keywords_and_reserved_generated_names() {
        for source in [
            "package class; abi 1; message M { value: u8; }",
            "package constexpr; abi 1; message M { value: u8; }",
            "package p; abi 1; struct struct { value: u8; }",
            "package p; abi 1; message class { value: u8; }",
            "package p; abi 1; message M { class: u8; }",
            "package p; abi 1; primitive P:u8 { class = 1; }",
        ] {
            let error = compile_str(source, "keyword.rbt").unwrap_err();
            assert!(error.to_string().contains("reserved by Rust 2024/C++20"));
        }

        let error = compile_str(
            "package p; abi 1; message M { IOX2_TYPE_NAME: u8; }",
            "iox2-name.rbt",
        )
        .unwrap_err();
        assert!(error.to_string().contains("IOX2_TYPE_NAME"));
    }

    #[test]
    fn rejects_generated_top_level_name_collisions() {
        let constant_collision = compile_str(
            "package p; abi 1; primitive P:u32 { M_DESCRIPTOR = 1; } message M { value: u8; }",
            "constant-collision.rbt",
        )
        .unwrap_err();
        assert!(
            constant_collision
                .to_string()
                .contains("generated top-level name")
        );

        let descriptor_collision = compile_str(
            "package p; abi 1; message fooBar { value: u8; } message foo_bar { value: u16; }",
            "descriptor-collision.rbt",
        )
        .unwrap_err();
        assert!(
            descriptor_collision
                .to_string()
                .contains("generated top-level name")
        );
    }

    #[test]
    fn generated_cpp_descriptors_carry_signatures_and_newtypes_are_asserted() {
        let schema = compile_str(
            "package p; abi 1; primitive P:u32 { ONE = 1; } message M { value: P; }",
            "generated.rbt",
        )
        .unwrap();
        let rust = generate_rust(&schema);
        assert!(rust.contains("size_of::<P>() == 4"));
        assert!(rust.contains("align_of::<P>() == 4"));

        let cpp = generate_cpp(&schema);
        assert!(cpp.contains("inline constexpr char M_SIGNATURE[]"));
        assert!(cpp.contains(", M_SIGNATURE};"));
    }

    #[test]
    fn generated_headers_start_with_stable_version_metadata() {
        let schema =
            compile_str("package p; abi 1; message M { value: u32; }", "header.rbt").unwrap();
        let expected = format!(
            "// @generated by rbt-idl; deterministic output.\n// IDL language version: {IDL_LANGUAGE_VERSION}\n// Message descriptor format version: {MESSAGE_DESCRIPTOR_VERSION}\n// DO NOT EDIT.\n"
        );
        let rust = generate_rust(&schema);
        assert!(rust.starts_with(&expected));
        let rust_lines = rust.lines().collect::<Vec<_>>();
        for (index, line) in rust_lines.iter().enumerate() {
            if line.starts_with("unsafe impl") {
                assert!(index > 0 && rust_lines[index - 1].starts_with("// SAFETY:"));
            }
        }
        assert!(generate_cpp(&schema).starts_with(&expected));
        let python_expected = format!(
            "# @generated by rbt-idl; deterministic output.\n# IDL language version: {IDL_LANGUAGE_VERSION}\n# Message descriptor format version: {MESSAGE_DESCRIPTOR_VERSION}\n# DO NOT EDIT.\n"
        );
        assert!(generate_python(&schema).starts_with(&python_expected));
        let descriptor = schema.schema_descriptor_json();
        assert!(descriptor.contains("\"idl_language_version\": 1"));
        assert!(descriptor.contains("\"message_descriptor_version\": 1"));
        assert!(!descriptor.contains("\"version\""));
    }

    #[test]
    fn generated_rust_handles_fixed_arrays_over_thirty_two_elements() {
        let schema = compile_str(
            "package p; abi 1; struct Small { bytes: u8[32]; } struct Large { bytes: u8[64]; } message LargeMessage { values: u16[48]; }",
            "large-arrays.rbt",
        )
        .unwrap();
        let rust = generate_rust(&schema);

        assert!(rust.contains(
            "#[derive(Clone, Copy, Debug, Default)]\npub struct Small {\n    pub bytes: [u8; 32],"
        ));
        assert!(rust.contains(
            "#[derive(Clone, Copy, Debug)]\npub struct Large {\n    pub bytes: [u8; 64],\n}\nimpl Default for Large {\n    fn default() -> Self {\n        Self {\n            bytes: [Default::default(); 64],"
        ));
        assert!(rust.contains(
            "const ENCODED_SIZE: usize = <u8 as crate::CanonicalFieldCodec>::ENCODED_SIZE * 64;"
        ));
        assert!(rust.contains(
            "for value in &self.bytes {\n            crate::CanonicalFieldCodec::encode_canonical(value, output);"
        ));
        assert!(rust.contains(
            "for value in &mut self.bytes {\n            crate::CanonicalFieldCodec::decode_canonical(value, cursor)?;"
        ));
        assert!(rust.contains(
            "#[derive(Clone, Copy, Debug)]\npub struct LargeMessage {\n    pub values: [u16; 48],\n}\nimpl Default for LargeMessage {\n    fn default() -> Self {\n        Self {\n            values: [Default::default(); 48],"
        ));
    }

    #[test]
    fn layout_hash_input_excludes_generator_and_sdk_metadata() {
        let schema =
            compile_str("package p; abi 1; message M { value: u32; }", "hash.rbt").unwrap();
        let resolved =
            resolve(&parse("package p; abi 1; message M { value: u32; }", "hash.rbt").unwrap())
                .unwrap();
        let message = resolved.message("M").unwrap();
        let signature = schema.message("M").unwrap().signature.as_str();
        let description = layout_hash_description(&resolved, message, signature);
        assert!(!description.contains("rbt-idl"));
        assert!(!description.contains("sdk"));
        assert!(!description.contains(env!("CARGO_PKG_VERSION")));
        assert_eq!(
            layout_hash_description(&resolved, message, signature),
            description
        );
        assert_eq!(
            message_layout_hash(&schema, "M").unwrap().to_string(),
            schema.message("M").unwrap().layout_hash
        );
    }

    #[test]
    fn rejects_duplicate_cpp_slice_bindings() {
        let error = compile_str(
            "package p; abi 1; struct H { value: u8; } message First = slice { header H; payload u16[]; } message Second = slice { header H; payload u16[]; }",
            "slice-binding-collision.rbt",
        )
        .unwrap_err();
        assert!(error.to_string().contains("C++ binding collision"));
        assert!(error.to_string().contains("First"));
    }

    #[test]
    fn all_backends_are_byte_deterministic() {
        let schema = compile_str(SENSOR, "one.rbt").unwrap();
        assert_eq!(generate_rust(&schema), generate_rust(&schema));
        assert_eq!(generate_cpp(&schema), generate_cpp(&schema));
        assert_eq!(generate_python(&schema), generate_python(&schema));
        let point_schema = compile_str(
            "package rbt.sensor; abi 1; struct H { count: u32; } struct PointXYZI { x: f32; y: f32; z: f32; intensity: f32; } message PointCloud = slice { header H; payload PointXYZI[]; }",
            "points.rbt",
        )
        .unwrap();
        let python = generate_python(&point_schema);
        assert!(python.contains("POINT_XYZI_DTYPE_SPEC"));
        assert!(python.contains("\"offsets\": (0, 4, 8, 12)"));
        assert!(python.contains("\"itemsize\": 16"));
        assert!(python.contains("\"layout_hash\":"));
        assert_eq!(
            schema.schema_descriptor_json(),
            schema.schema_descriptor_json()
        );
    }

    #[test]
    fn canonical_file_schema_reparses_without_changing_descriptors() {
        let schema = compile_str(SENSOR, "source.rbt").unwrap();
        let canonical = schema.canonical_idl();
        let reparsed = compile_str(&canonical, "embedded.rbt").unwrap();
        assert_eq!(canonical, reparsed.canonical_idl());
        assert_eq!(
            schema.schema_descriptor_json(),
            reparsed.schema_descriptor_json()
        );

        let rust = generate_rust(&schema);
        assert!(rust.contains("IMAGE_FILE_SCHEMA"));
        assert!(rust.contains("impl crate::CanonicalSliceMessage for Image"));
        assert!(rust.contains("impl crate::CanonicalFieldCodec for Header"));
    }

    #[test]
    fn public_idl_formatter_is_idempotent() {
        let schema = compile_str(
            "package p; abi 1; newtype Kind: u16 { A = 1; } struct Header { count: u32; } message M = slice { header Header; payload Kind[]; }",
            "source.rbt",
        )
        .unwrap();
        let formatted = format_idl(&schema);
        assert_eq!(formatted, schema.format_idl());
        let reparsed = compile_str(&formatted, "formatted.rbt").unwrap();
        assert_eq!(formatted, format_idl(&reparsed));
    }
}

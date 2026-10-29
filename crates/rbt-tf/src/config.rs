//! Strict parsing for bounded, user-supplied static extrinsics.

use std::collections::{BTreeMap, BTreeSet};

use serde::de::{DeserializeSeed, IgnoredAny, MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};

use crate::{FrameId, Quaternion, StaticEdge, StaticSet, TfError, TfResult, Transform3, Vec3};
use rbt_time::Timestamp;

/// Maximum size accepted for a TOML static-extrinsics document.
pub const MAX_STATIC_CONFIG_BYTES: usize = 1024 * 1024;

const MAX_STATIC_TRANSFORMS: usize = 4096;

/// A fully parsed and validated set of configured static extrinsics.
#[derive(Clone, Debug, PartialEq)]
pub struct StaticExtrinsicsConfig {
    edges: Vec<StaticEdge>,
}

impl StaticExtrinsicsConfig {
    /// Parses and validates a strict TOML static-extrinsics document.
    ///
    /// The document uses `[[transforms.static]]` entries with `parent`,
    /// `child`, `translation_m`, and `rotation_xyzw` fields. Unknown fields,
    /// invalid frames or transforms, duplicate children, cycles, oversized
    /// input, and transform counts above `max_transforms` are rejected before
    /// a config value is returned.
    pub fn from_toml(source: &str, max_transforms: usize) -> TfResult<Self> {
        if source.len() > MAX_STATIC_CONFIG_BYTES {
            return Err(TfError::SnapshotTooLarge);
        }
        if max_transforms > MAX_STATIC_TRANSFORMS {
            return Err(TfError::InvalidConfig);
        }

        let raw = ConfigSeed { max_transforms }
            .deserialize(toml::de::Deserializer::new(source))
            .map_err(|_| TfError::InvalidConfig)?;
        let edges = raw
            .transforms
            .into_iter()
            .map(RawEdge::into_edge)
            .collect::<TfResult<Vec<_>>>()?;

        validate_graph(&edges)?;
        Ok(Self { edges })
    }

    /// Returns the complete validated static edge set.
    pub fn edges(&self) -> &[StaticEdge] {
        &self.edges
    }

    /// Consumes this config into one static set with caller-supplied timing.
    pub fn into_static_set(
        self,
        revision: u64,
        effective_from: Timestamp,
        effective_uncertainty_ns: Option<u64>,
    ) -> StaticSet {
        StaticSet {
            revision,
            effective_from,
            effective_uncertainty_ns,
            transforms: self.edges,
        }
    }
}

struct RawConfig {
    transforms: Vec<RawEdge>,
}

struct ConfigSeed {
    max_transforms: usize,
}

impl<'de> DeserializeSeed<'de> for ConfigSeed {
    type Value = RawConfig;

    fn deserialize<D>(self, deserializer: D) -> Result<Self::Value, D::Error>
    where
        D: Deserializer<'de>,
    {
        deserializer.deserialize_map(ConfigVisitor {
            max_transforms: self.max_transforms,
        })
    }
}

struct ConfigVisitor {
    max_transforms: usize,
}

impl<'de> Visitor<'de> for ConfigVisitor {
    type Value = RawConfig;

    fn expecting(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("a static extrinsics configuration table")
    }

    fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
    where
        M: MapAccess<'de>,
    {
        let mut transforms = None;
        while let Some(key) = map.next_key::<String>()? {
            match key.as_str() {
                "transforms" if transforms.is_none() => {
                    transforms = Some(map.next_value_seed(TransformsSeed {
                        max_transforms: self.max_transforms,
                    })?);
                }
                "transforms" => {
                    return Err(serde::de::Error::duplicate_field("transforms"));
                }
                _ => return Err(serde::de::Error::unknown_field(&key, &["transforms"])),
            }
        }
        let transforms = transforms.ok_or_else(|| serde::de::Error::missing_field("transforms"))?;
        Ok(RawConfig { transforms })
    }
}

struct TransformsSeed {
    max_transforms: usize,
}

impl<'de> DeserializeSeed<'de> for TransformsSeed {
    type Value = Vec<RawEdge>;

    fn deserialize<D>(self, deserializer: D) -> Result<Self::Value, D::Error>
    where
        D: Deserializer<'de>,
    {
        deserializer.deserialize_map(TransformsVisitor {
            max_transforms: self.max_transforms,
        })
    }
}

struct TransformsVisitor {
    max_transforms: usize,
}

impl<'de> Visitor<'de> for TransformsVisitor {
    type Value = Vec<RawEdge>;

    fn expecting(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("a transforms table containing a static array")
    }

    fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
    where
        M: MapAccess<'de>,
    {
        let mut static_edges = None;
        while let Some(key) = map.next_key::<String>()? {
            match key.as_str() {
                "static" if static_edges.is_none() => {
                    static_edges = Some(map.next_value_seed(StaticEdgesSeed {
                        max_transforms: self.max_transforms,
                    })?);
                }
                "static" => return Err(serde::de::Error::duplicate_field("static")),
                _ => return Err(serde::de::Error::unknown_field(&key, &["static"])),
            }
        }
        static_edges.ok_or_else(|| serde::de::Error::missing_field("static"))
    }
}

struct StaticEdgesSeed {
    max_transforms: usize,
}

impl<'de> DeserializeSeed<'de> for StaticEdgesSeed {
    type Value = Vec<RawEdge>;

    fn deserialize<D>(self, deserializer: D) -> Result<Self::Value, D::Error>
    where
        D: Deserializer<'de>,
    {
        deserializer.deserialize_seq(StaticEdgesVisitor {
            max_transforms: self.max_transforms,
        })
    }
}

struct StaticEdgesVisitor {
    max_transforms: usize,
}

impl<'de> Visitor<'de> for StaticEdgesVisitor {
    type Value = Vec<RawEdge>;

    fn expecting(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("a bounded array of static transforms")
    }

    fn visit_seq<S>(self, mut sequence: S) -> Result<Self::Value, S::Error>
    where
        S: SeqAccess<'de>,
    {
        let capacity = sequence.size_hint().unwrap_or(0).min(self.max_transforms);
        let mut transforms = Vec::with_capacity(capacity);
        while transforms.len() < self.max_transforms {
            let Some(edge) = sequence.next_element()? else {
                return Ok(transforms);
            };
            transforms.push(edge);
        }

        if sequence.next_element::<IgnoredAny>()?.is_some() {
            return Err(serde::de::Error::custom(
                "static transform count exceeds configured limit",
            ));
        }
        Ok(transforms)
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawEdge {
    parent: String,
    child: String,
    translation_m: [f64; 3],
    rotation_xyzw: [f64; 4],
}

impl RawEdge {
    fn into_edge(self) -> TfResult<StaticEdge> {
        let parent = FrameId::new(&self.parent)?;
        let child = FrameId::new(&self.child)?;
        let translation = Vec3::new(
            self.translation_m[0],
            self.translation_m[1],
            self.translation_m[2],
        )?;
        let rotation = Quaternion::new(
            self.rotation_xyzw[0],
            self.rotation_xyzw[1],
            self.rotation_xyzw[2],
            self.rotation_xyzw[3],
        )?;
        Ok(StaticEdge {
            parent,
            child,
            transform: Transform3::new(translation, rotation),
        })
    }
}

fn validate_graph(edges: &[StaticEdge]) -> TfResult<()> {
    let mut parents = BTreeMap::new();
    for edge in edges {
        if edge.parent == edge.child {
            return Err(TfError::CycleDetected);
        }
        if parents.insert(edge.child, edge.parent).is_some() {
            return Err(TfError::ConflictingParent);
        }
    }

    let mut complete = BTreeSet::new();
    for child in parents.keys().copied() {
        if complete.contains(&child) {
            continue;
        }
        let mut path = BTreeSet::new();
        let mut frame = child;
        while let Some(parent) = parents.get(&frame).copied() {
            if complete.contains(&frame) {
                break;
            }
            if !path.insert(frame) {
                return Err(TfError::CycleDetected);
            }
            frame = parent;
        }
        complete.extend(path);
    }
    Ok(())
}

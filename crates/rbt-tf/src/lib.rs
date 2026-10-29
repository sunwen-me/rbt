//! Local, bounded spatial transforms built on rbt identities and real-hardware time.
#![forbid(unsafe_code)]

mod buffer;
pub mod config;
mod error;
mod frame;
mod math;
mod model;
pub mod wire;

pub use buffer::TransformBuffer;
pub use config::{MAX_STATIC_CONFIG_BYTES, StaticExtrinsicsConfig};
pub use error::{TfError, TfResult};
pub use frame::FrameId;
pub use math::{Pose3, Quaternion, Transform3, Vec3};
pub use model::*;
pub use wire::*;

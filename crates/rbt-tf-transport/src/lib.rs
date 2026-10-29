//! Native, bounded TF transport adapters over the existing rbt stream and RPC APIs.
#![forbid(unsafe_code)]

mod dynamic;
mod error;
mod lifecycle;
mod static_tf;

pub use dynamic::{DrainReport, DynamicTfPublisher, DynamicTfSubscriber};
pub use error::{TransportError, TransportResult};
pub use lifecycle::{
    DynamicTfBinding, ProvidedDynamicTf, ProvidedStaticTf, StaticTfBinding, TransformActivationExt,
};
pub use static_tf::{
    StaticServeOutcome, StaticTfClient, StaticTfProvider, StaticTfProviderControl,
};

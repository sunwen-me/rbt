use thiserror::Error as ThisError;

pub use rbt_compose::{ComponentError, ControlPlaneError, RequirementError, RpcEndpointError};
pub use rbt_iox2::{RpcError, StreamError};
pub use rbt_tf::TfError;
pub use rbt_tf_transport::TransportError as TfTransportError;
pub use rbt_time::TimeError;

/// Aggregate error for facade operations spanning multiple runtime
/// subsystems.
#[derive(Debug, ThisError)]
pub enum RbtError {
    /// A component lifecycle operation failed.
    #[error(transparent)]
    Component(#[from] ComponentError),
    /// A component requirement could not be resolved.
    #[error(transparent)]
    Requirement(#[from] RequirementError),
    /// A local stream operation failed.
    #[error(transparent)]
    Stream(#[from] StreamError),
    /// A typed local RPC data-plane operation failed.
    #[error(transparent)]
    Rpc(#[from] RpcError),
    /// A lifecycle-bound RPC requirement or server endpoint could not open.
    #[error(transparent)]
    RpcEndpoint(#[from] RpcEndpointError),
    /// A Composer control-plane operation failed.
    #[error(transparent)]
    ControlPlane(#[from] ControlPlaneError),
    /// A robot timestamp or clock mapping operation failed.
    #[error(transparent)]
    Time(#[from] TimeError),
    /// A local spatial query or graph update failed.
    #[error(transparent)]
    Tf(#[from] TfError),
    /// A native TF endpoint or activation binding failed.
    #[error(transparent)]
    TfTransport(#[from] TfTransportError),
}

/// Result type used by facade helpers which may cross subsystem boundaries.
pub type Result<T> = std::result::Result<T, RbtError>;

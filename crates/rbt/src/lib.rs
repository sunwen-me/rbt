//! The user-facing Rust SDK for the robotics building blocks runtime.
//!
//! The lower-level workspace crates remain available for advanced runtime and
//! transport work. Application components should normally depend on this crate
//! and use the small modules below instead of reaching into those internals.

#![doc(html_root_url = "https://docs.rs/rbt/0.8.0")]

pub mod error;

/// Clock identities, timestamps, and explicit clock mappings.
pub mod time {
    pub use rbt_time::*;
}

/// In-sample timing metadata carried by ordinary native byte-slice streams.
pub mod time_stream {
    pub use rbt_time_stream::*;
}

/// Validated spatial math, bounded local history, and native TF endpoints.
pub mod tf {
    pub use rbt_tf::*;
    pub use rbt_tf_transport::*;
}

/// Version of the installed Rust SDK package.
pub const SDK_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Component lifecycle APIs.
pub mod component {
    pub use rbt_compose::{
        ActivationCtx, Component, ComponentError, ControlPlaneStatus, EffectScope,
        RequirementError, RpcEndpointError, RuntimeEvent, ScopedTaskHandle, TaskOutcome,
        run_component,
    };
}

/// Typed, bounded local request/response APIs.
pub mod rpc {
    pub use rbt_iox2::{
        FixedSliceRpc, FixedSliceRpcClient, FixedSliceRpcRequestLoan, FixedSliceRpcResponseGuard,
        FixedSliceRpcServer, PendingRpcCall, Rpc, RpcCallCancelHandle, RpcClient, RpcClientMetrics,
        RpcClientWakeHandle, RpcDescriptor, RpcError, RpcPressure, RpcReadinessProtocol,
        RpcRequest, RpcRequestLoan, RpcResourceConfig, RpcResponseGuard, RpcResponseLoan,
        RpcResult, RpcServer, RpcServerMetrics, RpcServerWakeHandle, RpcTransportFeatures,
        SliceFixedRpc, SliceFixedRpcClient, SliceFixedRpcRequestLoan, SliceFixedRpcResponseGuard,
        SliceFixedRpcServer, SliceRpc, SliceRpcClient, SliceRpcRequestLoan, SliceRpcResponseGuard,
        SliceRpcServer, preflight_fixed_slice_rpc, preflight_rpc, preflight_slice_fixed_rpc,
        preflight_slice_rpc,
    };
}

/// Composer control-plane APIs.
pub mod control {
    pub use rbt_compose::{
        COMPOSER_SNAPSHOT_VERSION, CONTROL_PROTOCOL_VERSION, ComposerPhase, ControlPlaneError,
        serve,
    };
}

/// Typed local data-plane APIs.
pub mod stream {
    pub use rbt_iox2::{
        CONTROL_QUEUE_DEPTH, EventSource, LoanedSample, LoanedSliceSample, MAX_BORROWED_SAMPLES,
        MAX_DRAIN_PER_SOURCE, MAX_LOANED_SAMPLES, PressureEvent, Publisher, PublisherPressure,
        ReadyEvent, ReadyEventKind, ReceivedSample, ReceivedSliceSample, SENSOR_QUEUE_DEPTH,
        STREAM_HISTORY_SIZE, SlicePublisher, SliceStream, SliceSubscriber, Stream, StreamError,
        StreamPressure, StreamResourceConfig, StreamSemanticConfig, Subscriber, SubscriberPressure,
        UninitLoanedSliceSample, WaitContext, WaitError, WaitExit, WaitProgress, WaitStats,
        WaitStatsSnapshot, WaitToken, WaitWakeHandle, local_pressure_snapshots,
    };
}

/// Stable graph and message-description types used by components.
pub mod types {
    pub use rbt_core::{
        ActivationId, Binding, Capability, CapabilityDescriptor, CapabilityInstanceId, ComponentId,
        ComponentState, ProcessInstanceId, ProvidedCapability, Requirement, RequirementSpec,
        ResolvedBinding, Revision,
    };
    pub use rbt_types::{
        ControlCommand, Detection, FixedMessageAbi, Frame, Image, ImageHeader, LayoutHash,
        MESSAGE_DESCRIPTOR_VERSION, MessageShape, MessageTypeDescriptor, PixelFormat, PointCloud,
        PointCloudHeader, PointXYZI, ReadinessProtocol, ShmSafe, SliceMessageAbi,
        StaticMessageTypeDescriptor, StreamDescriptor, StreamProfile, TransportFeatures,
    };
}

pub use error::{
    ComponentError, ControlPlaneError, RbtError, RequirementError, Result, RpcEndpointError,
    RpcError, StreamError, TfError, TfTransportError, TimeError,
};

/// The intentionally small set of names used by most component applications.
pub mod prelude {
    pub use crate::{
        Result,
        component::{ActivationCtx, Component},
        rpc::{FixedSliceRpc, Rpc, SliceFixedRpc, SliceRpc},
        stream::Stream,
        types::{Capability, Requirement, StreamProfile},
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::prelude::{
        ActivationCtx, Capability, Component, Requirement, Result, Rpc, Stream, StreamProfile,
    };
    use rbt_types::Frame;
    use std::error::Error as _;
    use std::marker::PhantomData;

    #[test]
    fn sdk_version_matches_package_version() {
        assert_eq!(SDK_VERSION, env!("CARGO_PKG_VERSION"));
    }

    #[test]
    fn aggregate_result_preserves_subsystem_sources() {
        let component = ComponentError::preparation(std::io::Error::other("load model"));
        let error = RbtError::from(component);
        assert!(error.source().is_some());
    }

    #[test]
    fn prelude_exports_the_component_surface() {
        fn assert_component_surface<C, E>(_: PhantomData<(C, E)>)
        where
            C: Component<Config = (), Error = E>,
            E: std::fmt::Display + Send + Sync + 'static,
        {
            let _: Option<ActivationCtx<'_>> = None;
            let _: Option<Requirement<NeverCapability>> = None;
            let _: Option<Stream<Frame>> = None;
            let _: Option<Rpc<Frame, Frame>> = None;
            let _: Option<StreamProfile> = None;
            let _: Option<Result<()>> = None;
        }

        // The generic assertion intentionally only checks that the exported
        // names can be used as bounds; no component implementation is needed.
        assert_component_surface::<NeverComponent, NeverError>(PhantomData);
        fn assert_capability<C: Capability>() {}
        let _ = assert_capability::<NeverCapability>;
    }

    #[test]
    fn rpc_dynamic_surface_is_reexported() {
        use crate::prelude::{
            FixedSliceRpc as PreludeFixedSliceRpc, SliceFixedRpc as PreludeSliceFixedRpc,
            SliceRpc as PreludeSliceRpc,
        };
        use crate::rpc::{
            FixedSliceRpc, FixedSliceRpcClient, FixedSliceRpcRequestLoan,
            FixedSliceRpcResponseGuard, FixedSliceRpcServer, SliceFixedRpc, SliceFixedRpcClient,
            SliceFixedRpcRequestLoan, SliceFixedRpcResponseGuard, SliceFixedRpcServer, SliceRpc,
            SliceRpcClient, SliceRpcRequestLoan, SliceRpcResponseGuard, SliceRpcServer,
            preflight_fixed_slice_rpc, preflight_slice_fixed_rpc, preflight_slice_rpc,
        };
        use crate::types::{Frame, Image};

        let _: Option<FixedSliceRpc<Frame, Image>> = None;
        let _: Option<FixedSliceRpcClient<Frame, Image>> = None;
        let _: Option<FixedSliceRpcRequestLoan<Frame, Image>> = None;
        let _: Option<FixedSliceRpcResponseGuard<Image>> = None;
        let _: Option<FixedSliceRpcServer<Frame, Image>> = None;
        let _: Option<SliceFixedRpc<Image, Frame>> = None;
        let _: Option<SliceFixedRpcClient<Image, Frame>> = None;
        let _: Option<SliceFixedRpcRequestLoan<Image, Frame>> = None;
        let _: Option<SliceFixedRpcResponseGuard<Frame>> = None;
        let _: Option<SliceFixedRpcServer<Image, Frame>> = None;
        let _: Option<SliceRpc<Image, Image>> = None;
        let _: Option<SliceRpcClient<Image, Image>> = None;
        let _: Option<SliceRpcRequestLoan<Image, Image>> = None;
        let _: Option<SliceRpcResponseGuard<Image>> = None;
        let _: Option<SliceRpcServer<Image, Image>> = None;
        let _: Option<PreludeFixedSliceRpc<Frame, Image>> = None;
        let _: Option<PreludeSliceFixedRpc<Image, Frame>> = None;
        let _: Option<PreludeSliceRpc<Image, Image>> = None;

        let _: fn(&rbt_types::RpcDescriptor) -> rbt_iox2::RpcResult<()> =
            preflight_fixed_slice_rpc::<Frame, Image>;
        let _: fn(&rbt_types::RpcDescriptor) -> rbt_iox2::RpcResult<()> =
            preflight_slice_fixed_rpc::<Image, Frame>;
        let _: fn(&rbt_types::RpcDescriptor) -> rbt_iox2::RpcResult<()> =
            preflight_slice_rpc::<Image, Image>;
    }

    struct NeverComponent;

    struct NeverCapability;

    impl Capability for NeverCapability {
        const TYPE_NAME: &'static str = "Never";
    }

    #[derive(Debug)]
    struct NeverError;

    impl std::fmt::Display for NeverError {
        fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            formatter.write_str("never")
        }
    }

    impl std::error::Error for NeverError {}

    #[async_trait::async_trait]
    impl Component for NeverComponent {
        type Config = ();
        type Error = NeverError;

        async fn prepare(_: &Self::Config) -> std::result::Result<Self, Self::Error> {
            Ok(Self)
        }

        fn id(&self) -> rbt_core::ComponentId {
            rbt_core::ComponentId::from("never")
        }

        fn requirements(&self) -> Vec<rbt_core::RequirementSpec> {
            Vec::new()
        }

        async fn activate(
            &mut self,
            _: &mut ActivationCtx<'_>,
        ) -> std::result::Result<(), Self::Error> {
            Ok(())
        }
    }
}

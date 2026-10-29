# RBT developer documentation — v0.8

These guides describe the current **rbt / SDK 0.8.0** release for Linux
real-hardware applications in Rust, C++20, and Python. Start with the path that
matches your work; the milestone reports below retain the versions and results
of their original runs.

This repository covers the framework runtime, SDKs, IDL, time and TF APIs, and
Composer/CLI. Robot-specific drivers, navigation, and perception live in the
separate robot workspace, at `../rbt_robot_ws/` from the repository root in the
local development layout.

## Build an application

1. Follow the [quickstart](quickstart.md) to install the tools, generate a
   project, check it, and run the first graph.
2. Read [concepts](concepts.md), [project layout](project-layout.md), and
   [component authoring](component-authoring.md) for requirements, activation,
   task ownership, and provider replacement.
3. Use the [Rust SDK](rust-sdk.md), [C++ SDK](cpp-sdk.md), or
   [Python SDK](python.md) reference for your implementation language.
4. Choose the feature guide below and a runnable [example](../examples/README.md).
   Use [CLI](cli.md) and [debugging](debugging.md) when the graph needs diagnosis.

## Feature guides

| Task | Guide |
| --- | --- |
| Understand control, composition, and native payload paths | [Architecture](architecture.md) |
| Define messages and regenerate language bindings | [IDL](idl.md), [local message ABI](abi.md) |
| Size native loans, queues, and retained samples | [Stream pressure](pressure.md), [pressure tuning](pressure-tuning.md), [runtime pressure policy](runtime-pressure.md) |
| Wait for data and shut down activation work | [Event-driven waiting](waiting.md) |
| Serve typed local request/response capabilities | [RPC](rpc.md) |
| Identify capture times and convert clock domains explicitly | [Real-hardware time](time.md) |
| Publish transforms and query bounded local history | [TF](tf.md), [TF architecture](tf-architecture.md), [static config example](tf-config-example.toml) |
| Own Python component tasks and native guards | [Python Component runtime](python-component.md), [asyncio runtime](python-asyncio.md) |
| Record and replay the supported message types | [MCAP](mcap.md) |
| Inspect graphs, RPC, streams, time, and local TF diagnostics | [CLI](cli.md), [debugging](debugging.md) |
| Check independent version and native compatibility | [Compatibility](compatibility.md) |

## Develop the framework

The [framework development guide](development.md) covers repository ownership,
source bootstrap, builds, generated artifacts, focused checks, and release
verification. The [distribution guide](distribution.md) covers local SDK,
source, and Python wheel bundles. [Testing](testing.md) describes gate scope and
retained evidence; [benchmarks](benchmarks.md) explains measurement methods.

The local robot workspace contains the Orbbec Gemini 335 and M3T integration
guides under `../rbt_robot_ws/drivers/orbbec/` and
`../rbt_robot_ws/perception/m3t/`.

The native baseline is the maintained **iceoryx2 0.10.0-rbt.2** source, shared by
Rust, C++, and Python. See its [build and provenance guide](../third_party/iceoryx2-rbt/README.md)
before changing middleware or installing a C++ prefix.

| Independent version axis | Current value |
| --- | --- |
| Product / SDK | `0.8.0` |
| IDL language / message descriptor | `1` / `1` |
| Composer control protocol / snapshot | `2` / `4` |
| MCAP file encoding | `1` (`rbt-canonical-v1`) |
| Native maintained build | `0.10.0-rbt.2` |

The [TF unsafe review](tf-unsafe-review.md) records the approved safety boundary:
49 legacy production locations plus 24 generated TF declarations, 73 total,
with no new handwritten unsafe. SDK versions do not change frozen layout
hashes or these independent protocol axes automatically.

## Release evidence and retained history

The [v0.8 TF report](tf-report.md) records accepted software and release-artifact
verification. Physical-camera TF and physical dynamic-TF acceptance remain
**NOT RUN**. Generic TF MCAP recording/replay remains **NOT VERIFIED**; inherited
MCAP test results apply to the supported recording types.

Reports describe the source, environment, commands, and limitations at the time
of each run. They are evidence rather than installation instructions for the
current release. Historical failures and unexecuted hardware checks retain
their original status.

| Area | Reports and investigations |
| --- | --- |
| Spatial transforms, v0.8 | [TF report](tf-report.md), [generated unsafe review](tf-unsafe-review.md) |
| Real-hardware time, introduced v0.7 | [Time report](time-report.md), [pre-change audit](time-audit.md) |
| Typed RPC, introduced v0.6 | [RPC report](rpc-report.md), [native RPC investigation](rpc-iox2-notes.md), [implementation handoff](RPC_HANDOFF.md) |
| Current maintained native source | [Unified iceoryx2 acceptance](iceoryx2-unified-report.md) |
| Developer tooling, v0.5 | [DX report](dx-report.md) |
| Resource configuration and pressure, v0.4 | [Pressure tuning report](pressure-tuning-report.md), [historical 0.9.3 pressure investigation](runtime-pressure-notes.md) |
| Recording and playback, v0.3 | [MCAP report](mcap-report.md), [format investigation](mcap-notes.md) |
| Python receive and publishing | [Receive SDK report](python-data-sdk-report.md), [publisher report](python-publisher-report.md) |
| Python lifecycle and asyncio | [Component report](python-component-report.md), [asyncio report](python-asyncio-report.md) |
| Event-driven waiting | [WaitSet report](waitset-report.md), [native waiting investigation](waitset-notes.md) |
| Original runtime and interop | [v0.1 report](v0.1-report.md), [historical Phase 4 verification](phase4-verification.md) |
| Earlier network experiments | [Initial fork report](iceoryx2-rbt-report.md), [cross-host notes](cross-host-iox2-notes.md) and [report](cross-host-iox2-report.md), [upstream notes](cross-host-upstream-notes.md) and [report](cross-host-upstream-report.md) |

The network experiments do not provide an RBT application network gateway or
cross-host TF. Simulator time, Parameters, calibration solvers, and a global TF
query service remain outside v0.8. See [Time](time.md), [TF](tf.md), and
[compatibility](compatibility.md) for the supported boundaries.

## Keeping these docs current

Update the relevant guide and this index when a public command, module, schema,
or lifecycle contract changes. Check claims against source and command help;
keep version axes independent. Preserve historical reports and link fresh
evidence instead of rewriting earlier results. The
[development guide](development.md#documentation-changes) describes the
documentation check scope.

# Runtime CLI

Runtime graph-inspection commands ask Composer for an immutable snapshot over
the control protocol. Composer is the composition authority; the CLI never
scans `/dev/shm` to guess a graph. Inspection is read-only and does not
increment the composition revision. `time status` reads local clocks, while
TF commands can also use an explicitly selected local diagnostic snapshot.

Set `RBT_COMPOSER_ADDR` to override `127.0.0.1:7447`.

## The local-project workflow

For a new application, use this order:

```bash
rbt doctor
rbt new hello-robot
cd hello-robot
rbt check
rbt idl check
rbt idl fmt --check
rbt idl generate
rbt run
```

`rbt doctor` is read-only and may be run outside a project. `rbt new` refuses
to overwrite a target. `rbt check` is a fast static validation and never
starts Composer. `rbt run` is only a bounded local process launcher; Composer
and the component runtimes remain the sources of lifecycle and data-plane
truth. See [Quickstart](quickstart.md), [Project layout](project-layout.md),
and [Component authoring](component-authoring.md).

The project-aware commands are:

| Command | Purpose | JSON | Needs Composer | Needs project |
| --- | --- | --- | --- | --- |
| `rbt --version` | Show product and SDK versions; this form must stand alone | no | no | no |
| `rbt version --verbose` | Show product, SDK, and frozen compatibility axes | no | no | no |
| `rbt help <command>` | Show built-in help for a command | no | no | no |
| `rbt doctor` | Check local prerequisites and project context | `--json` | no | optional |
| `rbt new <name>` | Create a self-contained project | no | no | no |
| `rbt new component <name>` | Add a component template | no | no | yes |
| `rbt new message <Name>` | Add a source IDL declaration | no | no | yes |
| `rbt check` | Validate manifest, executables, IDL, and generated outputs | `--json` | no | yes |
| `rbt idl check` | Parse and compile project IDL | no | no | yes |
| `rbt idl fmt [--check]` | Deterministically format project IDL | no | no | yes |
| `rbt idl generate` | Refresh configured bindings and manifest | no | no | yes |
| `rbt run` | Own Composer and configured component children | line output | yes (started by launcher) | yes |
| `rbt rpc info <selector>` | Inspect an active RPC capability | `--json` | yes | no |
| `rbt time status` | Inspect local host clocks and explicit source observations | `--json` | no | no |
| `rbt tf status` / `rbt tf tree` | Inspect TF provider metadata or known local graph edges | `--json` | unless `--snapshot` is used | no |

`--project /path/to/rbt.toml` may precede project-aware commands. From a
subdirectory, discovery walks upward to the nearest manifest. Exit classes are
intentionally finite:

| Code | Class | Meaning |
| ---: | --- | --- |
| `0` | success | The command completed successfully. |
| `1` | operational | An I/O, runtime, MCAP, or other operation failed. |
| `2` | usage | The command or its options are malformed. |
| `3` | validation | Project, IDL, configuration, or doctor validation failed. |
| `4` | unavailable | Composer, a required executable, or another prerequisite is unavailable. |

`--json` keeps supported command output machine-readable; warnings and progress
go to stderr.

## Version and environment diagnostics

Use `rbt --version` by itself for a short version check, or
`rbt version --verbose` when filing a compatibility report. The verbose output
reports the SDK, IDL language, message descriptor, control protocol, Composer
snapshot, MCAP file encoding, and iceoryx2 axes separately. The current frozen
values are IDL 1, descriptor 1, protocol 2, snapshot 4, MCAP encoding 1
(`rbt-canonical-v1`), and iceoryx2 `0.10.0-rbt.2`; product and SDK are 0.8.0.
Version output has no JSON mode. `rbt doctor --json` uses stable `check_id`
values rather than requiring a script to parse English sentences. Normal
errors explain what happened, where, and the suggested action. Add global
`--verbose` or the relevant `RUST_BACKTRACE` setting only when a full cause
chain is needed.

## Command help

`rbt help <command>` prints the built-in synopsis and scope for one command.
For example, `rbt help tf` explains the difference between Composer metadata
and a selected local TF diagnostic snapshot. The equivalent top-level form
`rbt <command> --help` is also supported.

## `rbt rpc info`

Inspect an active request/response capability through Composer:

```bash
rbt rpc info <capability-instance-uuid>
rbt rpc info <physical-endpoint>
rbt rpc info <capability-type>       # only when exactly one is active
rbt rpc info <capability-instance-uuid> --verbose
rbt rpc info <capability-instance-uuid> --json
```

The selector accepts a capability instance UUID, an exact physical endpoint,
or an exact semantic type when that type has one active instance. Ambiguous or
inactive selectors fail with guidance to list capabilities. Human output shows
request and response ABI names and layout hashes, readiness, bounded resource
limits, and the availability/freshness of server and client metrics. The
default view shortens the UUID and hides the physical endpoint; `--verbose`
and JSON expose full identity.

## `rbt time status`

```bash
rbt time status
rbt time status --json
rbt time status --sources observations.json --json
```

This command reads the local Linux monotonic and realtime clocks and does not
need Composer. `--sources PATH` adds one explicitly selected, bounded JSON
snapshot of source clock observations and optional calibration. No sources are
discovered automatically, and supplied calibration is reported as an
observation rather than proof of clock authority or device synchronization.
Unknown capture timestamps stay unknown unless a verified acquisition source
supplied them. The JSON view preserves clock identity, generation, uncertainty,
mapping status, and freshness for each known source.

## `rbt tf status` and `rbt tf tree`

```bash
rbt tf status
rbt tf status --json
rbt tf tree --json
rbt tf status --snapshot consumer-tf.json
rbt tf tree --snapshot consumer-tf.json
```

Without `--snapshot`, these commands read TF capability descriptors and
bindings from Composer. `status` reports providers, message descriptors, and
consumer bindings; `tree` reports known payload edges. Composer does not
expose TF payload graph/history, so Composer-only graph visibility is partial
and the edge list is unavailable rather than an empty graph. `--snapshot`
selects one bounded consumer-local diagnostic observation file; it can report
the selected buffer graph and lookup metrics without Composer. The file is
diagnostic input, not TF payload transport, and is never discovered
automatically.

## `rbt ps`

```bash
rbt ps
rbt ps --full
rbt ps --json
```

The table reports `COMPONENT`, process identity, activation id, uppercase
lifecycle state, and control connection. The header also reports Composer
phase, epoch, and revision. `--full` keeps full UUIDs and expands requirements;
`--json` returns the typed `PsOutput` projection used by tests and automation:
`epoch`, `revision`, `phase`, `components`, and `process_conflicts`. Each
component carries full process/activation IDs, state and failure/block detail,
connection status, pending work, and `lifecycle_age_ms` (which is `null` when
the v3 snapshot does not provide a transition timestamp). It is not the raw
internal `InspectionView` type. The additive `stream_pressures` component field
preserves typed component-owned pressure records, including subscriber-only
records, which remain useful when a provider capability has already
disappeared.

## `rbt graph`

```bash
rbt graph
rbt graph --dot
rbt graph --json
```

Text edges have the form:

```text
camera-a -[camera:Camera:Stream]-> detector
detector -[detections:Detections:Stream]-> viewer
```

Only satisfied requirement edges appear in the compact text view. `--dot`
produces a Graphviz digraph; `--json` returns typed component, requirement, and
capability nodes, typed edges, and a `freshness` object. JSON keeps unresolved
edges rather than silently dropping diagnostic context.

## `rbt why`

```bash
rbt why detector
rbt why detector --json
```

Start here for a `BLOCKED` or `FAILED` component. Output distinguishes current
reason/cause, pending Composer work, missing requirements, active or available
providers, the last active process/activation and bindings, and duplicate
process quarantine. Last-active context is retained after teardown so failure
diagnosis does not lose the provider/capability identity that caused it.
The JSON form is a `WhyOutput`: use `reasons[].code` (and the convenience
`reason_codes` array) as stable keys, not the human detail text.

## `rbt capability list`

```bash
rbt capability list
rbt capability list Camera
rbt capability list --provider camera
rbt capability list Camera --provider camera --verbose
rbt capability list --json
```

The optional positional selector is an exact semantic capability type; the
`--provider` selector is an exact provider component. Only capabilities backed
by a current active activation are listed. The default human view keeps the
logical type/provider/message/profile and a shortened instance ID; `--verbose`
adds the full instance ID and physical endpoint. JSON always retains full
identity fields and accepts the same filters.

## `rbt stream info`

```bash
rbt stream info <capability-instance-uuid>
rbt stream info <physical-endpoint>
rbt stream info <capability-type>       # only when exactly one is active
rbt stream info <capability-instance-uuid> --verbose
```

The selector may be a capability instance UUID, an exact physical endpoint, or
an exact semantic capability type when that type has exactly one active
instance. A type selector with multiple active instances is rejected as
ambiguous; use `rbt capability list --verbose` to choose an instance. Physical
endpoints are useful for diagnostics but should not be copied into project
configuration. This reports provider, semantic type, physical endpoint,
qualified message name, layout hash, fixed/slice shape, profile, readiness protocol,
header/element size and alignment, frozen queue/history/safe-overflow
semantics, and the descriptor's `max_loans`/`max_borrowed` resource metadata.
The default human output hides the physical endpoint and shortens the instance
ID; `--verbose` and JSON expose it. Values from a live provider report are
labelled as such. When only the descriptor is known, pressure is unavailable
rather than displayed as zero.

## `rbt stream pressure`

```bash
rbt stream pressure <capability-instance-uuid>
rbt stream pressure <physical-endpoint>
rbt stream pressure <capability-type>    # only when exactly one is active
```

`stream pressure` uses the same logical-selector/instance/physical-endpoint
resolution rules as `stream info`; it does not accept `--verbose` because its
human output already shows transport detail. This reports the provider's latest
low-rate transport snapshot: resources,
loan attempts/successes/current/high-water/failures, payload byte observations,
send/delivery/readiness counters, aggregate connected subscribers, subscriber
observations, snapshot age/status, and typed pressure assessments. `--json`
exposes stable identity, profile, resources, snapshot timestamp/freshness, and
bounded subscriber observations. Exact Sensor drops and individual subscriber
queue occupancy are not exposed by the current iceoryx2 `0.10.0-rbt.2` pressure
interface, so those counters remain `unknown` rather than being inferred.
Reports are associated with the
provider process and physical service, making their counter lifetime one
`CapabilityInstanceId`; the display freshness cutoff is 2.5 seconds.

## `rbt record`

Record a fixed snapshot of one or more supported active capabilities:

```bash
rbt record --output run.mcap --capability Camera
rbt record --output run.mcap --capability Camera --capability MotionCommand
rbt record --output mixed.mcap --all
rbt record --output bounded.mcap --all --buffer-bytes 67108864 --duration 10
```

Selectors match a capability type, provider component, exact capability UUID,
or exact physical endpoint and must be unambiguous. The recorder is an
ordinary side subscriber. Its writer queue has a byte budget (64 MiB by
default); Sensor overflow is counted and marks the file incomplete, while a
Control overflow is a terminal error. SIGINT/SIGTERM drains and finalizes the
file before printing one JSON summary.

## `rbt mcap info`

Inspect and validate a recording without Composer:

```bash
rbt mcap info run.mcap
rbt mcap info run.mcap --json
```

The output includes file size, time range/duration, schemas, channels,
encodings, exact layout hashes, message and payload counts, slice maxima, and
recording/drop metadata. Corrupt containers, unsupported encodings, and exact
ABI/schema mismatches exit nonzero.

## `rbt play`

Publish supported channels as fresh native Component capabilities:

```bash
rbt play run.mcap
rbt play run.mcap --rate 2.0
rbt play run.mcap --rate 0
rbt play external.mcap --skip-unsupported
```

The default rate is 1.0; 2.0 is twice speed and 0 is unpaced. Playback needs
Composer, performs complete schema/ABI/payload preflight before creating any
publisher, schedules all channels globally from MCAP `log_time`, and preserves
file order for ties. `--skip-unsupported` applies only to unrelated external
channels; malformed rbt data still fails. See [MCAP recording and
playback](mcap.md) for identity, timestamp, codec, and no-retry rules.

## JSON stability and failures

JSON is intended for scripts such as `soak_v01.sh` and `verify_mcap.sh`;
consumers should still
treat new fields as additive during 0.x. A connection failure is an inspection
error, not proof that component data paths stopped. A control/snapshot version
mismatch is reported as a protocol diagnostic and never imported as state.

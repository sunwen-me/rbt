# RBT v0.8.0 quickstart

This is the canonical developer workflow for a fresh v0.8.0 Linux x86_64 checkout.
It assumes Rust 1.89+ is installed and that the `rbt` executable is on
`PATH` (an installed prefix is the normal setup).  A C++ compiler and Python
are optional for this first Rust-only graph.

The goal is a useful graph in under ten minutes:

```text
doctor -> new -> check -> run -> inspect -> diagnose -> stop
```

The first page deliberately avoids requiring knowledge of Composer epochs,
activation identities, physical service names, or shared-memory internals.
Start from the [documentation index](README.md) and
[development guide](development.md). Those details are covered by
[Architecture](architecture.md), the [Rust SDK](rust-sdk.md), and
[Message ABI](abi.md).

## 1. Check the machine

Run the doctor from any directory, including outside a project:

```bash
rbt --version
rbt version --verbose
rbt doctor
rbt doctor --json
```

Use `rbt --version` by itself for the short product/SDK version. The verbose
compatibility axes use the separate `rbt version --verbose` command.

`PASS`, `WARN`, and `SKIP` are useful information; optional C++ and Python
checks do not prevent a Rust-only project from being created.  A `FAIL`
includes the check id, the reason, and the next action.  `--json` writes only
the structured result to stdout, so it is safe to pipe into CI tooling.

## 2. Create and validate a project

Use an empty directory (the command refuses to overwrite an existing target):

```bash
mkdir -p ~/rbt-work
cd ~/rbt-work
rbt new hello-robot
cd hello-robot
rbt check
rbt idl check
rbt idl fmt --check
rbt idl generate
cargo build --workspace
```

`rbt new` creates a real Cargo workspace with a small source/sink graph.  The
generated project contains no repository-relative dependency or `target/`
assumption; an installed SDK is selected automatically.  For an SDK built
from a checkout, set `RBT_RUST_SDK` to the SDK root before creating the project.

The generated README is intentionally short.  `rbt check` is the fast,
side-effect-free pre-commit/CI check: it parses `rbt.toml`, checks component
entries and executable paths, validates IDL, and verifies that generated files
match the generation manifest.

## 3. Run the first graph

`rbt run` is the local process launcher.  It starts the configured Composer and
components, prefixes child output, prints low-rate lifecycle transitions, and
owns every child until shutdown:

```bash
rbt run
```

Leave this terminal running.  A healthy generated graph reports the source and
sink as `ACTIVE` and prints `HELLO sequence=...` lines.  In another terminal,
from the project root, inspect the same graph:

```bash
rbt ps
rbt graph
rbt why hello-sink
rbt capability list
```

`rbt why` is useful even when the component is healthy: it shows the requirement
and its current provider.  When a logical capability type has one active
instance, use that type directly for the detailed stream views:

```bash
rbt stream info Hello
rbt stream pressure Hello
```

If more than one provider exposes the same logical type, choose an instance
with `rbt capability list --verbose`.  A capability UUID is live diagnostic
identity, not project configuration, and should never be copied into
`rbt.toml`.  The default inspection views hide physical endpoint names and
shorten UUIDs, while `--json` and `--verbose` expose the complete machine-facing
identity when an integration needs it.

## 4. Add a message and a component

The generated source/sink pair is enough to prove the installation.  To start
authoring application code, add a schema and two components:

```bash
rbt new message Detection
rbt new component camera
rbt new component detector
rbt check
rbt idl generate
cargo build --workspace
```

`rbt new message` creates the `.rbt` source of truth; it never asks you to edit
generated Rust or C++ bindings.  `rbt new component` creates a small,
cancellation-aware `Component` template.  See
[Component authoring](component-authoring.md) for the normal requirement,
binding, capability, and task patterns.

## 5. Understand a failure and recover

The important runtime behavior is visible with the same three read-only
commands.  Stop the provider process using the `started hello-source pid=...`
line printed by the launcher, then inspect:

```bash
rbt ps --full
rbt why hello-sink
rbt graph
```

The sink should be shown as `BLOCKED` with an unresolved `Hello` requirement
and the last active provider retained as diagnostic context.  For this local
exercise, start the built provider in another foreground terminal (use the
same `RBT_COMPOSER_ADDR` if you overrode it), then inspect once more:

```bash
target/debug/hello-source
rbt ps
rbt graph
```

The component returns through the normal activation path.  Preparation is
process-lifetime work; replacing a provider does not require recreating the
consumer process.  Press Ctrl-C in the replacement-provider terminal before
the final launcher shutdown.  The deterministic mixed-language provider
replacement scenario is available as an advanced regression:

```bash
./scripts/test_mixed_runtime.sh
```

The generated source publishes the semantic `Hello` capability using the
frozen `Image` message ABI, so the same live graph can complete the durable-data
workflow without a hand-written schema:

```bash
rbt record --output hello.mcap --capability Hello --duration 1.5
rbt mcap info hello.mcap
rbt play hello.mcap --rate 0
```

`record` and `play` print JSON summaries; progress stays on stderr. The file
uses the existing `rbt-canonical-v1` MCAP encoding.

## 6. Stop cleanly

Press Ctrl-C in the `rbt run` terminal.  The launcher sends a bounded graceful
shutdown to components and Composer, waits for every child, and exits.  A
second Ctrl-C takes the documented force-stop path if a child is stuck.  The
launcher never detaches children or relies on shell job-control state.

## What to read next

- [Documentation index](README.md) is the v0.8 guide map.
- [Development guide](development.md) covers contributor setup and the
  aggregate verification workflow.
- [Project layout](project-layout.md) explains `rbt.toml`, generated files,
  and discovery from a subdirectory.
- [Component authoring](component-authoring.md) is the compact API guide.
- [IDL](idl.md) covers the schema grammar and deterministic backends.
- [Time](time.md), [TF](tf.md), and [RPC](rpc.md) cover clock identity,
  transform diagnostics, and request/response capabilities.
- [Debugging](debugging.md) maps common failures to commands and actions.
- [Stream pressure](pressure.md) explains `BorrowLimitReached`,
  `LoanPoolExhausted`, and `QueueFull` without suggesting unsafe tuning.
- [MCAP recording and playback](mcap.md) covers `record`, `mcap info`, and
  `play`.

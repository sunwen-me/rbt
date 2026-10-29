# Project layout

The v0.8.0 `rbt new` command creates a self-contained local developer project.
The exact SDK path is resolved when the project is created, while project
truth remains in the repository itself. A typical generated tree is:

```text
hello-robot/
├── rbt.toml
├── Cargo.toml
├── README.md
├── .gitignore
├── types/
│   ├── hello.rbt
│   ├── Cargo.toml
│   └── src/lib.rs             # generated Rust bindings index
├── generated/
│   ├── rust/
│   ├── cpp/
│   ├── descriptor/
│   └── python/
└── components/
    ├── hello-source/
    │   ├── Cargo.toml
    │   └── src/main.rs
    └── hello-sink/
        ├── Cargo.toml
        └── src/main.rs
```

The generated project is a real Cargo workspace.  `rbt check` and
`rbt idl generate` are the supported project entry points; editing a generated
binding by hand is not.

## `rbt.toml`

The project file is a small developer-tool manifest, not a second runtime
protocol.  It describes the project name, build command, IDL directories, the
local Composer command, and the component processes that `rbt run` owns.  It
does not redefine message layout, ABI hashes, capability identity, queue
policy, or lifecycle semantics.

A generated file has this shape (the CLI writes the exact SDK version and
paths):

```toml
schema_version = 1

[project]
name = "hello-robot"
sdk_version = "<installed-sdk-version>"

[build]
command = ["cargo", "build", "--workspace"]

[idl]
sources = ["types"]
rust = "generated/rust"
cpp = "generated/cpp"
descriptor = "generated/descriptor"
python = "generated/python"

[run]
startup_timeout_seconds = 15
shutdown_timeout_seconds = 10

[[component]]
name = "hello-source"
package = "hello-source"

[[component]]
name = "hello-sink"
package = "hello-sink"
```

For a non-Rust executable, a component uses an explicit argv instead of a
Cargo package:

```toml
[[component]]
name = "external-viewer"
command = ["./bin/external-viewer", "--config", "viewer.toml"]
environment = { RBT_LOG = "info" }
```

Each component must specify exactly one of `package` or `command`.  Duplicate
logical component names, missing packages, and missing command executables are
reported by `rbt check` before Composer is started.

## Generated files and ownership

IDL files under `types/` are hand-authored schema truth.  The generator writes
the configured Rust, C++, descriptor, and optional Python output directories,
plus `generated/generation-manifest.json`.  The manifest records source hashes,
packages, output stems, generator version, and expected files.  This lets
`rbt check` distinguish a stale generated tree from a malformed schema.

Generated files carry a stable `@generated` header and should be committed when
the project is distributed as source.  A clean checkout can regenerate them
with:

```bash
rbt idl check
rbt idl fmt --check
rbt idl generate
```

Only outputs listed by the previous generation manifest may be removed during
regeneration; unrelated files are preserved.  Output stems must be unique
across all IDL packages, so a collision is a validation error rather than a
silent overwrite.

## Discovery and explicit roots

From a project subdirectory, `rbt check`, `rbt idl ...`, `rbt run`, and
project-aware `rbt doctor` walk upward until they find `rbt.toml`, like Cargo
does for `Cargo.toml`:

```bash
cd components/hello-sink/src
rbt check
```

CI and editor integrations can choose an explicit manifest without changing
the current working directory:

```bash
rbt --project /path/to/hello-robot/rbt.toml check
rbt --project /path/to/hello-robot/rbt.toml idl generate
```

No hidden project state is stored in a global config directory.  `RBT_RUST_SDK`
is an explicit development override; installed projects resolve the staged
SDK beside the `rbt` executable.

## What is intentionally not in this file

Deployment hosts, SSH, containers, restart policies, secrets, health-probe
DSLs, and schema evolution are outside the v0.8.0 local developer manifest.
Composer remains the composition authority and the component runtime remains
responsible for lifecycle, binding, capability ownership, and cleanup.

## What to read next

- [Documentation index](README.md) maps the v0.8.0 guides.
- [Development guide](development.md) covers contributor setup and verification.
- [Quickstart](quickstart.md) walks through a generated project and first graph.
- [Time](time.md), [TF](tf.md), and [RPC](rpc.md) describe clock identity,
  transform diagnostics, and request/response capabilities.

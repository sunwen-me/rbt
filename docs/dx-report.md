# v0.5 developer-experience report

This document is the acceptance record and contract for the local developer
workflow. It intentionally keeps runtime semantics, message ABI, and the
existing verification gates separate from developer tooling. Run
`./scripts/verify_dx.sh --quick` for the bounded onboarding gate and
`./scripts/verify_dx.sh --full` for the onboarding gate plus inherited release
checks. The verifier stores command output under `target/dx-logs/` and prints
the exact path on failure.

The observed 2026-09-27 acceptance evidence is the DX full run at
`target/dx-logs/dx-full-20260927T124130Z-53632/`, its console transcript at
`target/dx-full-console-3.log`, and the inherited pressure run at
`target/pressure-tuning-verify-logs/pressure-tuning-full-20260927T124228Z-78301/`.
The retained quick-run evidence is
`target/dx-logs/dx-quick-20260927T041311Z-24681/`.

## Scope and invariants

The v0.5 surface adds local project scaffolding, IDL maintenance, prerequisite
diagnostics, and a process-owning launcher. It does not add a network gateway,
schema evolution, ROS bridge, GUI/TUI/LSP, central logging service, or a new
runtime protocol. `rbt run` is a launcher; Composer remains the composition
authority and payloads never pass through it.

The compatibility axes that must remain unchanged are:

| Axis | Required value |
| --- | --- |
| CLI/SDK milestone | `0.5.0` |
| IDL language | `1` |
| Message descriptor format | `1` |
| Control protocol | `2` |
| Composer snapshot | `3` |
| MCAP/file encoding | `1` (`rbt-canonical-v1`) |
| iceoryx2 | `0.9.3` |
| production unsafe baseline | `49`; observed DX delta is `0` |
| Image layout hash | `943fa4008cf0f55f89a281b051c69a6f` |
| PointCloud layout hash | `d9da2bb29bb2cf217c40f1a56d1fb755` |
| ControlCommand layout hash | `9381cf15842a48ac0964111f0f45ef72` |

The full run's `version-verbose.log` reports `rbt`/SDK `0.5.0`, IDL language
`1`, message descriptor `1`, control protocol `2`, Composer snapshot `3`, MCAP
file encoding `1`, and iceoryx2 `0.9.3`. Its MCAP metadata and the inherited
benchmark logs observe the exact Image, PointCloud, and ControlCommand hashes
above. The pressure gate's unsafe audit reports
`RBT_PRESSURE_TUNING_UNSAFE expected=49 observed=49 delta=0`; the inherited
MCAP audit likewise reports `old=49 new=49 delta=0`.

## First-ten-minutes acceptance flow

This is a clean temporary-directory workflow, not a claim about human typing
speed. The same sequence is documented in [Quickstart](quickstart.md). The
DX full run used a project path containing spaces and passed every row below;
its retained evidence is
`target/dx-logs/dx-full-20260927T124130Z-53632/`.

| Step | Command | Expected result | Observed result |
| --- | --- | --- | --- |
| 1 | `rbt doctor` | Required tools are `PASS`; optional SDKs are `WARN`/`SKIP` when unused | PASS: human and structural JSON checks |
| 2 | `rbt new hello-robot` | A new project is created without overwriting an existing path | PASS: creation plus overwrite refusal |
| 3 | `cd hello-robot && rbt check` | Project is valid without starting Composer | PASS: root and nested discovery |
| 4 | `rbt idl check` | All source schemas compile with source locations on errors | PASS: one generated schema checked |
| 5 | `rbt idl fmt --check` | Canonical formatting is clean and non-mutating | PASS: formatting clean before and after generation |
| 6 | `rbt idl generate && cargo build --workspace` | Generated bindings and all project packages build | PASS: four generated components built |
| 7 | `rbt run` | Composer and expected components start under one owned process tree | PASS: all four reached `ACTIVE` |
| 8 | `rbt ps --json && rbt graph --json` | Components/capabilities are visible in structured runtime truth | PASS: human and structural JSON checks |
| 9 | `rbt why <consumer>` | Requirement/provider explanation is actionable | PASS: active, provider-loss, and recovered states |
| 10 | Ctrl-C, then process audit | Children are reaped; no orphan or zombie remains | PASS: exact PID and process-group audits are clean |

The full workflow also creates a message and component, checks from a nested
directory, exercises `stream info`/`stream pressure`, validates JSON output,
records and replays an 8-message MCAP file, and runs the provider-loss and
replacement scenario. The observed full run reports clean graceful shutdown,
clean restart, and clean process-group audits.

## Public command matrix

| Command | Purpose | Human output | JSON support | Requires Composer? | Requires project? |
| --- | --- | --- | --- | --- | --- |
| `rbt --version` / `rbt version` | Independent version axes | concise or verbose (`version --verbose`) | no | no | no |
| `rbt doctor` | Machine/project readiness | `PASS`/`WARN`/`FAIL`/`SKIP` checks | `--json` | no | optional |
| `rbt new` | Project/component/message templates | created path and next action | no | no | no/yes for child forms |
| `rbt check` | Static project validation | diagnostics with path/line/action | `--json` | no | yes |
| `rbt idl check` | Parse/compile schemas | source diagnostics | no | no | yes |
| `rbt idl fmt` | Deterministic schema formatting | changed/check result | no | no | yes |
| `rbt idl generate` | Bindings and generation manifest | output summary | no | no | yes |
| `rbt run` | Local process ownership | line-oriented lifecycle | no | starts one | yes |
| `rbt ps` | Lifecycle/process snapshot | compact table | `--json` | yes | no |
| `rbt graph` | Typed topology projection | text or DOT | `--json` | yes | no |
| `rbt why` | One component's cause/context | reasons, candidates, suggestions | `--json` | yes | no |
| `rbt capability list` | Active capability inventory | logical fields by default | `--json` | yes | no |
| `rbt stream info` | ABI/profile/resource metadata | human summary | `--json` | yes | no |
| `rbt stream pressure` | Low-rate typed pressure snapshot | freshness and bounded metrics | `--json` | yes | no |
| `rbt record` | Bounded side-subscriber recording | progress + summary | summary JSON | yes | no |
| `rbt mcap info` | Offline file inspection | schemas/channels/counts | `--json` | no | no |
| `rbt play` | Preflighted replay | progress + summary | summary JSON | yes | no |

All `--json` results put machine-readable data only on stdout. Warnings,
progress, and diagnostics that are not part of the result are on stderr. Scripts
should use stable fields such as `check_id`, `status`, `summary`,
`suggested_action`, `reason_codes`, `reasons[].code`, `nodes`, `edges`,
`freshness`, `pressure`, `process_instance_id`, `activation_id`,
`lifecycle_freshness`, and `stream_pressures`, rather than parsing human tables.

## JSON contract examples

Doctor uses a typed report with stable diagnostic entries. Fields are additive
within the 0.x line:

```json
{
  "platform": "linux-x86_64",
  "checks": [
    {
      "check_id": "rust_sdk",
      "status": "PASS",
      "summary": "Rust SDK available (/installed/sdk/root)"
    }
  ],
  "success": true
}
```

Project `rbt check --json` reports `project_root`, `manifest`, `valid`, and
its diagnostic entries. Optional `details`, `suggested_action`, and source
location fields are omitted when unavailable.

The observed `rbt ps --json` projection is an object with `epoch`, `revision`,
`phase`, `components`, and `process_conflicts`. Each component carries the
full `process_instance_id`, numeric-or-null `activation_id`, uppercase
`state`, `lifecycle_age_ms`, `lifecycle_freshness`, and `stream_pressures`.
In `ps-active.json`, all four components were `ACTIVE`; lifecycle age was
`null` with `lifecycle_freshness: "age_unavailable"`, and the Hello source and
sink carried publisher/subscriber pressure observations. `ps-blocked.json`
observed `hello-sink` as `BLOCKED` with the provider-disappeared block reason.

Graph and why projections preserve full identity in JSON even when the human
view shortens it. Graph nodes have `kind`, `id`, `label`, `resolved`, and
component/requirement/capability fields; edges have `kind`, `source`,
`target`, and `resolved`; both include `freshness`. Why results include the
`reason_codes` convenience array and detailed `reasons` entries whose stable
selector is `reasons[].code`, plus current state, requirements, candidates,
pressure evidence, suggestions, and last-active context when the snapshot
contains it.

## Observed failure UX

| Failure | Useful result |
| --- | --- |
| Bad IDL | `broken-bad-idl.log` reports the source path, line/column `5:5`, caret, and `unknown type "floatt"`. |
| Missing component package | `broken-missing-component.log` reports `component_package_name_mismatch`, the Cargo path, and the actionable package-name suggestion. |
| Provider loss | `why-blocked.json` reports `requirement_unresolved`, `provider_disappeared`, and `runtime_reported`, with the retained binding and recovery suggestions; `ps-blocked.json` shows `BLOCKED`. |
| Pressure | `stream-pressure-hello.json` includes owner identity, bounded counters, `snapshot_freshness`, and subscriber observations; the human output keeps unavailable drop counts as `unknown`. |
| ABI mismatch | The Phase 4 evidence reports expected/actual qualified type and layout hash with `transport_opened=false`, rather than only `open failed`. |
| Corrupt MCAP | The MCAP gate rejects a truncated file with `MCAP file ended in the middle of a record` for both `mcap info` and `play`, before replay publication. |

The captured operational errors above printed no Rust backtrace. `--verbose`
and the documented environment debugging path retain access to the complete
cause chain.

## Process cleanup evidence

The launcher contract is bounded and line-oriented:

```text
Ctrl-C -> cancel components -> drain activation scopes -> stop Composer
       -> wait/reap every child -> exit
second signal or timeout -> terminate owned process groups -> reap -> nonzero
```

The verifier records the launcher PID, Composer PID, component PIDs, exit
statuses, and a post-shutdown `/proc` audit. Acceptance requires:

```text
Composer count = 0
child count = 0
zombies = 0
```

The DX full evidence records a clean graceful shutdown in `process-audit.txt`
and `process-group-audit.txt`. The full restart lane completed 1,000 bounded
JSON queries with Composer revision unchanged and file descriptors `14 -> 14`;
its exact PID and process-group audits are clean. The forced-shutdown lane
confirmed that the first SIGINT was held by a TERM-resistant wrapper and the
second SIGINT forced bounded nonzero shutdown; its exact PID and process-group
audits are also clean.

The launcher must not kill an unrelated process with a broad name match, leave
detached grandchildren, or silently turn an unexpected startup failure into a
successful run.

## Inherited gates

The inherited evidence is the pressure-tuning full run at
`target/pressure-tuning-verify-logs/pressure-tuning-full-20260927T124228Z-78301/`.
Its composition was `v01=full`, `waitset=full`, `mcap=quick`,
`python-publisher=full`, `python-component=quick`, `python-asyncio=quick`,
with mixed-pressure, the 300-second soak, and the benchmark matrix retained.
The direct child logs are
`target/pressure-tuning-verify-logs/pressure-tuning-full-20260927T124228Z-78301/inherited-v01.log`,
`target/pressure-tuning-verify-logs/pressure-tuning-full-20260927T124228Z-78301/inherited-waitset.log`,
`target/pressure-tuning-verify-logs/pressure-tuning-full-20260927T124228Z-78301/inherited-mcap.log`,
`target/pressure-tuning-verify-logs/pressure-tuning-full-20260927T124228Z-78301/inherited-python-publisher.log`,
`target/pressure-tuning-verify-logs/pressure-tuning-full-20260927T124228Z-78301/inherited-python-component.log`,
and `target/pressure-tuning-verify-logs/pressure-tuning-full-20260927T124228Z-78301/inherited-python-asyncio.log`.

The pressure child reports five passing sensor/control/graph cycles in 332
seconds, verified loan recovery and provider/composer restart, zero orphan
capabilities and stale bindings, 51 resource samples, and no obvious
unbounded growth. Its detailed child logs are under
`target/pressure-logs/pressure.ogfwKx/`. The run also records
`RBT_PRESSURE_TUNING_UNSAFE expected=49 observed=49 delta=0`; shellcheck was
explicitly skipped because it was not installed.

The inherited v0.1 evidence includes clean-prefix installation at
`/tmp/rbt-install-smoke.S7MxpP/prefix`, installed Rust and C++ consumer builds
and runs, a fresh installed-project/runtime acceptance, and a clean `/proc`
audit. Its C++ ASan/UBSan ctest passed all 6 tests; the Python publisher ASan
lane passed 37 tests. The full-run console and child logs retain those exact
commands and paths.

## Evidence commands

Run from the repository root:

```bash
./scripts/verify_dx.sh --quick
./scripts/verify_dx.sh --full
```

The script also performs shell syntax checks, builds the CLI, runs the
first-ten-minutes project flow in a temporary path containing spaces, checks
JSON structurally, and verifies that IDL formatting is idempotent. It does not
modify checked-in generated files or persistent user configuration.

The final observed records are `target/dx-logs/dx-full-20260927T124130Z-53632/`
and `target/dx-full-console-3.log`; the inherited records above provide the
release/runtime, installer, sanitizer, MCAP, Python, WaitSet, pressure, and
benchmark evidence.

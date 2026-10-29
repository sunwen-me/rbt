# Multi-Agent Orchestration Policy

## Repository scope

This repository owns the generic RBT framework and SDK. Robot drivers,
navigation, perception, and deployment records are maintained in the sibling
`../rbt_robot_ws` repository. Consult `PROJECT_STATUS.md` and
`docs/repository-organization.md` before changing repository boundaries.
Local compatibility symlinks into that workspace are ignored and must not be
added to framework commits or source releases.

This repository uses `gpt-6.1-sol` at `xhigh` reasoning effort as the primary orchestrator and `gpt-6-luna` at `max` reasoning effort for execution-oriented subagents.

## Sol owns decisions and acceptance

The primary Sol agent must understand the user's real requirement, analyze the repository and risks, make architecture and implementation-plan decisions, split work into bounded tasks, schedule and supervise Luna agents, review their summaries and actual changes, inspect `git diff`, integrate work, run final tests, and perform final acceptance.

Sol must retain work involving ambiguous requirements, architecture, major data-model changes, public API design, security or authorization decisions, data-loss risk, migration strategy, highly coupled cross-module design, redirection after a failed worker attempt, final code review, final integration, and final approval. Never delegate final architecture decisions or final acceptance to Luna.

## Delegate execution to Luna by default

For non-trivial requests, actively use the project `explorer` and `worker` agents without requiring the user to ask for delegation.

Prefer Luna for repository exploration, grep/search, locating entry points and call relationships, reading many files, finding existing patterns and tests, log analysis, bounded implementation, bug fixes, straightforward refactors, CRUD, boilerplate, unit and integration tests, lint, formatting, documentation updates, repetitive edits, and any implementation with clear scope and verifiable acceptance criteria.

Do not spend Sol's context on large mechanical searches, directory traversal, repetitive edits, formatting, or repetitive tests when a bounded Luna task can do them.

## Default workflow

For every non-simple task:

1. Sol interprets the request and identifies risks and missing repository knowledge.
2. Sol delegates focused read-only discovery to one or more `explorer` agents when useful.
3. Explorers return distilled evidence only.
4. Sol decides the design and creates bounded, independent task packets.
5. Sol delegates clear implementation and verification work to `worker` agents.
6. Sol waits for results, then inspects the actual files, `git diff`, and test evidence.
7. Sol runs any necessary integration or broader tests.
8. If a concrete defect remains, Sol normally sends the specific repair back to a worker, then reviews again.
9. Sol alone gives final acceptance.

## Bounded task packets

Do not copy the entire main conversation into a subagent. Give each Luna only the minimum useful packet:

- task goal
- allowed files or directories
- necessary background
- constraints and non-goals
- acceptance criteria
- tests or checks to run

Require concise summaries rather than raw logs or large file excerpts.

## Concurrency

Parallelize independent read-only exploration aggressively when it saves time or improves coverage. Parallelize write-heavy tasks cautiously. Never run multiple workers concurrently when they may edit the same file or highly overlapping code. Use serial execution for coupled changes.

## Verification contract

Sol must verify worker claims against the actual workspace. Review relevant diffs and test output, check for unrelated changes, and resolve integration issues before declaring completion. A worker's successful report is evidence, not final approval.

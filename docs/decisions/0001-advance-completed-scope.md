# Advance a Completed Scope

## Status

Accepted on 2026-07-19 by explicit user direction.

## Context

[`F01-S02`](../v1_docs/F01_项目初始化与开发_Harness/F01-S02_项目文档、状态与进度交接体系_步骤文档.md) makes `completed` terminal so an accepted scope cannot be reopened. The project state nevertheless needs a guarded way to move from that completed scope to a distinct next scope named by `next_actions`.

## Decision

Add `tools/project_state.py advance-scope` as a separate operation. It does not add outgoing transitions to `completed`. It requires:

- the current state to be `completed`;
- a different, explicit version, feature, and step;
- `--human-confirmed` with `--updated-by human`;
- an explicit replacement or clearing of `next_actions`;
- all target-state gates, including reviewed documents for `ready_for_implementation`, verification for `awaiting_acceptance`, and blockers for `blocked`.

The command validates the complete candidate state and atomically replaces `current.json`, matching the existing transition write contract.

## Consequences

- Completed scopes remain immutable and cannot be reopened through `transition`.
- Scope advancement is distinguishable from an ordinary state transition in command output and tests.
- A coordinator can reconcile an already integrated next scope without manually editing project state.

# Derive Next Actions from a Reviewed Roadmap

## Status

Accepted on 2026-07-21 by explicit user direction.

## Context

`state next` previously interpreted only the `next_actions` already stored in `current.json`. Transitions preserved that field and scope advancement required callers to replace or clear it, so an accepted scope could become `completed` with no mechanically discoverable successor even when the reviewed feature DAG named one.

## Decision

Maintain `docs/status/roadmap.json` as the machine-readable route graph. Every route is bound to a reviewed Markdown snapshot by repository path, source revision, and generated-body SHA-256. Validation rejects stale sources, malformed or cyclic routes, and actions without target routes.

When a scope becomes `completed`, `project_state.py` derives its exact `next_actions` from the validated route. Non-completed states carry no candidates. `advance-scope` accepts only a scope named by those derived actions. Manual next-action files and clearing flags are removed. `reconcile-roadmap` atomically repairs an existing state whose stored candidates predate this rule.

An explicit terminal route has an empty action object. A missing route is an error rather than an implicit terminal state.

## Consequences

- `state next` can distinguish zero, one, and multiple reviewed successors without relying on prior chat.
- Updating a reviewed design snapshot requires updating its roadmap revision/hash and routes in the same reviewed change.
- Human review and acceptance gates remain unchanged; the roadmap determines candidates, not whether a gate has passed.
- `current.json.next_actions` is cached, validated derived state rather than an independently authored planning field.

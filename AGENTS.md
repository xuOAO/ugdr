# Repository Agent Instructions

## Repository Map

- `docs/status/current.json`: machine-readable current scope, stable state, next actions, and blockers.
- `docs/status/roadmap.json`: reviewed-source-bound machine roadmap from which `next_actions` are derived.
- `docs/v1_docs/README.md`: index of reviewed design snapshots used for implementation.
- `docs/contracts/`: reviewed Client-visible API and behavioral contracts.
- `docs/decisions/`: durable decision records that do not belong in current state.
- `docs/progress/`: execution and verification records that do not belong in current state.
- `docs/architecture/repository-skeleton.md`: repository areas, targets, and dependency boundaries.
- `tools/client-contracts.json`: machine-readable Client contract, source revision, and public symbol inventory.
- `tools/check_client_contracts.py`: deterministic Client contract and libibverbs-alignment integration check.
- `tools/`: remaining deterministic project checks and state operations.

## Project Rules

- Feishu documents are the decision and review source. Repository Markdown files are reviewed implementation snapshots.
- Update `docs/status/current.json` only through `project_state.py transition`, `advance-scope`, or `reconcile-roadmap`; do not edit it by hand.
- Derive `next_actions` only from the validated roadmap; do not supply, clear, or infer them from chat or prose.
- Never infer human review, implementation confirmation, or acceptance from passing tests, commits, or prior chat context.
- Keep `tools/module-boundaries.json` and the generated section in `docs/architecture/repository-skeleton.md` synchronized when the skeleton changes.
- Keep durable decisions, current state, progress records, and temporary plans in their dedicated locations.

### RDMA Semantic Alignment

- Within the supported UGDR scope, align public APIs, object relationships, terminology, state transitions, error behavior, ordering, and completion semantics with standard RDMA/libibverbs behavior by default.
- Use conventional verbs terminology at public and design boundaries: QP contains SQ and RQ; applications post Send/Receive WRs; CQs yield WCs. Use WQE/CQE only for internal or provider-level representations unless an interface intentionally exposes them.
- Preserve operation-specific semantics, including the distinction between RDMA Write and RDMA Write With Immediate, receive-WR consumption, signaling, and local versus remote completion behavior.
- Do not claim support outside the documented UGDR subset. Any intentional divergence required by the daemon, queue, or GPU architecture must be explicit in the reviewed design, recorded as a durable decision, and covered by focused tests.

### Feishu Account Routing

- Before any `lark-cli` command or `lark-*` Skill operation, read `.agents/skills/lark-account-registry/SKILL.md` and resolve an account from `.lark`.
- When multiple accounts are eligible and the user did not name one, ask the user to select; do not choose or persist a selection automatically.
- Reuse the resolved `argv_prefix` for every Feishu command in the current task. Do not fall back to another profile after an authentication, scope, permission, or resource error.
- Never store Feishu secrets, tokens, cookies, passwords, or credentials in `.lark`.

## Validation

- Project state: `python3 tools/project_state.py validate --root .`
- Documentation governance: `python3 tools/check_project_docs.py --root .`
- Module boundaries after CMake configure: `python3 tools/check_module_boundaries.py --root . --build-dir build`
- Full configured test suite: `ctest --test-dir build --output-on-failure`

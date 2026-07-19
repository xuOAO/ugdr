# Repository Agent Instructions

## Repository Map

- `docs/status/current.json`: machine-readable current scope, stable state, next actions, and blockers.
- `docs/v1_docs/README.md`: index of reviewed design snapshots used for implementation.
- `docs/decisions/`: durable decision records that do not belong in current state.
- `docs/progress/`: execution and verification records that do not belong in current state.
- `docs/architecture/repository-skeleton.md`: repository areas, targets, and dependency boundaries.
- `tools/`: deterministic project checks and state operations.

## Project Rules

- Feishu documents are the decision and review source. Repository Markdown files are reviewed implementation snapshots.
- Update `docs/status/current.json` only through `python3 tools/project_state.py transition`; do not edit it by hand.
- Never infer human review, implementation confirmation, or acceptance from passing tests, commits, or prior chat context.
- Keep `tools/module-boundaries.json` and the generated section in `docs/architecture/repository-skeleton.md` synchronized when the skeleton changes.
- Keep durable decisions, current state, progress records, and temporary plans in their dedicated locations.

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

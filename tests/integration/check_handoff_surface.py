#!/usr/bin/env python3

import argparse
import json
import re
import sys
from pathlib import Path


STATES = {
    "awaiting_review",
    "ready_for_implementation",
    "awaiting_acceptance",
    "blocked",
    "completed",
}
LINK_RE = re.compile(r"\[[^\]]+\]\(([^)]+)\)")


def fail(errors) -> None:
    print("handoff-surface: FAIL", file=sys.stderr)
    for error in errors:
        print("- {}".format(error), file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agents", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--state", type=Path, required=True)
    args = parser.parse_args()

    errors = []
    try:
        agents = args.agents.read_text(encoding="utf-8")
        index = args.index.read_text(encoding="utf-8")
        state = json.loads(args.state.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail([str(error)])

    for value in (
        "docs/status/current.json",
        "docs/v1_docs/README.md",
        "python3 tools/project_state.py validate --root .",
    ):
        if value not in agents:
            errors.append("AGENTS.md does not expose {}".format(value))

    index_targets = [match.group(1) for match in LINK_RE.finditer(index)]
    if not any("F01-S02_" in target for target in index_targets):
        errors.append("v1 index does not link the reviewed F01-S02 snapshot")

    required = {"version", "feature", "step", "state", "next_actions", "blockers"}
    missing = sorted(required - set(state)) if isinstance(state, dict) else sorted(required)
    if missing:
        errors.append("current state is missing handoff fields: {}".format(", ".join(missing)))
    elif state["state"] not in STATES:
        errors.append("current state is not a stable state: {}".format(state["state"]))
    if isinstance(state, dict) and not isinstance(state.get("next_actions"), dict):
        errors.append("next_actions is not machine-readable")
    if isinstance(state, dict) and not isinstance(state.get("blockers"), list):
        errors.append("blockers is not machine-readable")

    if errors:
        fail(errors)
    print(
        "handoff-surface: OK (scope={}/{}/{}, state={}, action_groups={}, blockers={})".format(
            state["version"],
            state["feature"] or "-",
            state["step"] or "-",
            state["state"],
            len(state["next_actions"]),
            len(state["blockers"]),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

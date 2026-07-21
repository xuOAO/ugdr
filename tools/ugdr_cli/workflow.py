"""Deterministic project-state decisions and guarded state command delegation."""

import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Sequence, Tuple

import project_state

from .result import run_argv


WORKFLOW_EXIT_CODE = 35
WORKFLOW_STATES = frozenset(project_state.STATES)
ProcessRunner = Callable[[Sequence[str], Path, float], subprocess.CompletedProcess]


class WorkflowRulesError(Exception):
    """Raised when workflow-rules.json violates its stable contract."""


@dataclass(frozen=True)
class WorkflowDecision:
    state: str
    action: str
    requires_human: bool
    reason: str
    next_actions: Dict[str, object]

    def to_payload(self) -> Dict[str, object]:
        return {
            "command": "state.next",
            "ok": True,
            "state": self.state,
            "action": self.action,
            "requires_human": self.requires_human,
            "reason": self.reason,
            "next_actions": self.next_actions,
            "exit_code": 0,
        }


def _error_payload(command: str, reason: str, details=None) -> Dict[str, object]:
    payload = {
        "command": command,
        "ok": False,
        "reason": reason,
        "exit_code": WORKFLOW_EXIT_CODE,
    }
    if details is not None:
        payload["details"] = details
    return payload


def load_rules(root: Path) -> Dict[str, Dict[str, object]]:
    path = root / "tools/workflow-rules.json"
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise WorkflowRulesError(str(error)) from error
    if not isinstance(payload, dict) or payload.get("schema_version") != 1:
        raise WorkflowRulesError("schema_version must be 1")
    states = payload.get("states")
    if not isinstance(states, dict) or set(states) != WORKFLOW_STATES:
        raise WorkflowRulesError("states must define exactly the five stable states")
    for state, rule in states.items():
        if not isinstance(rule, dict) or set(rule) != {
            "action",
            "requires_human",
            "reason",
        }:
            raise WorkflowRulesError("{} has invalid rule fields".format(state))
        if not isinstance(rule["action"], str) or not rule["action"].strip():
            raise WorkflowRulesError("{}.action must be a non-empty string".format(state))
        if not isinstance(rule["requires_human"], bool):
            raise WorkflowRulesError("{}.requires_human must be boolean".format(state))
        if not isinstance(rule["reason"], str) or not rule["reason"].strip():
            raise WorkflowRulesError("{}.reason must be a non-empty string".format(state))
    return states


def state_show(root: Path) -> Tuple[Dict[str, object], int]:
    state, errors = project_state.validate_repository_state(root)
    if errors:
        payload = _error_payload("state.show", "project state is invalid", errors)
        return payload, WORKFLOW_EXIT_CODE
    payload = {
        "command": "state.show",
        "ok": True,
        "scope": {
            "version": state["version"],
            "feature": state["feature"],
            "step": state["step"],
        },
        "state": state["state"],
        "next_actions": state["next_actions"],
        "blockers": state["blockers"],
        "validation": "ok",
        "exit_code": 0,
    }
    return payload, 0


def _flatten_next_actions(next_actions: Dict[str, object]) -> List[object]:
    flattened = []
    for feature in sorted(next_actions):
        actions = next_actions[feature]
        flattened.extend(actions)
    return flattened


def state_next(root: Path) -> Tuple[Dict[str, object], int]:
    shown, exit_code = state_show(root)
    if exit_code:
        shown["command"] = "state.next"
        return shown, exit_code
    try:
        rules = load_rules(root)
    except WorkflowRulesError as error:
        payload = _error_payload("state.next", "workflow rules are invalid", str(error))
        return payload, WORKFLOW_EXIT_CODE

    state = str(shown["state"])
    rule = rules[state]
    action = str(rule["action"])
    requires_human = bool(rule["requires_human"])
    reason = str(rule["reason"])
    if state == "completed":
        actions = _flatten_next_actions(shown["next_actions"])
        if not actions:
            action = "no_next_action"
            requires_human = True
            reason = "当前 scope 的受审阅 roadmap route 明确为终点。"
        elif len(actions) == 1:
            action = "advance_scope"
            requires_human = False
            reason = "受审阅 roadmap 派生出唯一 next_action；当前继续意图可作为明确选择。"
        else:
            action = "select_next_action"
            requires_human = True
            reason = "受审阅 roadmap 派生出多个 next_actions，必须由用户明确选择。"

    decision = WorkflowDecision(
        state=state,
        action=action,
        requires_human=requires_human,
        reason=reason,
        next_actions=shown["next_actions"],
    )
    payload = decision.to_payload()
    if shown["blockers"]:
        payload["blockers"] = shown["blockers"]
    return payload, 0


def run_state_core(
    root: Path,
    subcommand: str,
    arguments: Sequence[str],
    runner: ProcessRunner = run_argv,
) -> Tuple[Dict[str, object], int]:
    command = [
        sys.executable,
        str(root / "tools/project_state.py"),
        subcommand,
        "--root",
        str(root),
    ] + list(arguments)
    try:
        process = runner(command, root, 30.0)
    except (OSError, subprocess.SubprocessError) as error:
        payload = _error_payload("state.{}".format(subcommand), "state core failed", str(error))
        return payload, WORKFLOW_EXIT_CODE

    raw = process.stdout if process.returncode == 0 else process.stderr
    try:
        result = json.loads(raw)
    except json.JSONDecodeError:
        result = {"diagnostic": raw.strip() or "state core returned no JSON"}
    payload = {
        "command": "state.{}".format(subcommand),
        "ok": process.returncode == 0,
        "result": result,
        "exit_code": 0 if process.returncode == 0 else WORKFLOW_EXIT_CODE,
    }
    if process.returncode:
        payload["reason"] = "state transition was rejected"
    return payload, int(payload["exit_code"])


def render_workflow_human(payload: Dict[str, object]) -> str:
    if not payload.get("ok"):
        return "FAIL {}: {} (exit_code={})".format(
            payload.get("command"), payload.get("reason"), payload.get("exit_code")
        )
    if payload.get("command") == "state.show":
        scope = payload["scope"]
        return "{}/{}/{}: {} (blockers={})".format(
            scope["version"],
            scope["feature"] or "-",
            scope["step"] or "-",
            payload["state"],
            len(payload["blockers"]),
        )
    if payload.get("command") == "state.next":
        return "{}: {} (requires_human={}) - {}".format(
            payload["state"],
            payload["action"],
            str(payload["requires_human"]).lower(),
            payload["reason"],
        )
    return json.dumps(payload["result"], ensure_ascii=False, indent=2)

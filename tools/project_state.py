#!/usr/bin/env python3

import argparse
import copy
import json
import os
import re
import sys
import tempfile
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import project_roadmap


STATE_KEYS = {
    "schema_version",
    "version",
    "feature",
    "step",
    "state",
    "next_actions",
    "blockers",
    "updated_at",
    "updated_by",
}
STATES = {
    "awaiting_review",
    "ready_for_implementation",
    "awaiting_acceptance",
    "blocked",
    "completed",
}
ALLOWED_TRANSITIONS = {
    "awaiting_review": {"ready_for_implementation", "blocked"},
    "ready_for_implementation": {"awaiting_review", "awaiting_acceptance", "blocked"},
    "awaiting_acceptance": {"ready_for_implementation", "completed", "blocked"},
    "blocked": {"awaiting_review", "ready_for_implementation", "awaiting_acceptance"},
    "completed": set(),
}
VERSION_RE = re.compile(r"^v[1-9][0-9]*$")
FEATURE_RE = re.compile(r"^F[0-9]{2}$")
STEP_RE = re.compile(r"^(F[0-9]{2})-S[0-9]{2}$")


class ProjectStateError(Exception):
    pass


def emit(payload: Dict, stream=sys.stdout) -> None:
    print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True), file=stream)


def issue(path: str, reason: str, value=None) -> Dict:
    result = {"path": path, "reason": reason}
    if value is not None:
        result["value"] = value
    return result


def load_json(path: Path) -> Dict:
    try:
        with path.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        raise ProjectStateError("{}: {}".format(path, error)) from error
    if not isinstance(value, dict):
        raise ProjectStateError("{}: expected a JSON object".format(path))
    return value


def resolve_inside(root: Path, value: str, field: str) -> Path:
    path = Path(value)
    if path.is_absolute():
        raise ProjectStateError("{} must be relative to the repository root".format(field))
    resolved_root = root.resolve()
    resolved = (resolved_root / path).resolve()
    if resolved != resolved_root and resolved_root not in resolved.parents:
        raise ProjectStateError("{} escapes the repository root: {}".format(field, value))
    return resolved


def parse_rfc3339(value: str) -> bool:
    candidate = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = datetime.fromisoformat(candidate)
    except ValueError:
        return False
    return parsed.tzinfo is not None


def validate_next_actions(value) -> List[Dict]:
    errors = []
    if not isinstance(value, dict):
        return [issue("$.next_actions", "must be an object", value)]

    for feature, actions in value.items():
        feature_path = "$.next_actions.{}".format(feature)
        if not isinstance(feature, str) or not FEATURE_RE.fullmatch(feature):
            errors.append(issue(feature_path, "key must match Fxx", feature))
        if not isinstance(actions, list):
            errors.append(issue(feature_path, "must be an array", actions))
            continue
        if not actions:
            continue

        all_strings = all(isinstance(item, str) for item in actions)
        all_objects = all(isinstance(item, dict) for item in actions)
        if not all_strings and not all_objects:
            errors.append(
                issue(feature_path, "must contain only step strings or only action objects", actions)
            )
            continue

        seen_steps = set()
        for index, action in enumerate(actions):
            action_path = "{}[{}]".format(feature_path, index)
            if all_strings:
                step = action
            else:
                unknown = sorted(set(action) - {"step", "action"})
                missing = sorted({"step", "action"} - set(action))
                if unknown:
                    errors.append(issue(action_path, "unknown fields: {}".format(", ".join(unknown))))
                if missing:
                    errors.append(issue(action_path, "missing fields: {}".format(", ".join(missing))))
                step = action.get("step")
                description = action.get("action")
                if not isinstance(description, str) or not description.strip():
                    errors.append(issue(action_path + ".action", "must be a non-empty string", description))

            match = STEP_RE.fullmatch(step) if isinstance(step, str) else None
            if match is None:
                errors.append(issue(action_path, "step must match Fxx-Sxx", step))
                continue
            if match.group(1) != feature:
                errors.append(issue(action_path, "step must belong to feature {}".format(feature), step))
            if step in seen_steps:
                errors.append(issue(action_path, "duplicate step", step))
            seen_steps.add(step)
    return errors


def validate_state_value(state: Dict, schema: Dict) -> List[Dict]:
    errors = []
    schema_version = schema.get("properties", {}).get("schema_version", {}).get("const")
    if schema_version != 1:
        errors.append(issue("$schema.properties.schema_version.const", "must be 1", schema_version))

    unknown = sorted(set(state) - STATE_KEYS)
    missing = sorted(STATE_KEYS - set(state))
    for key in unknown:
        errors.append(issue("$.{}".format(key), "unknown field", state.get(key)))
    for key in missing:
        errors.append(issue("$.{}".format(key), "required field is missing"))

    if state.get("schema_version") != 1:
        errors.append(issue("$.schema_version", "must be 1", state.get("schema_version")))

    version = state.get("version")
    if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
        errors.append(issue("$.version", "must match vN", version))

    feature = state.get("feature")
    if not isinstance(feature, str) or (feature and not FEATURE_RE.fullmatch(feature)):
        errors.append(issue("$.feature", "must be empty or match Fxx", feature))

    step = state.get("step")
    step_match = STEP_RE.fullmatch(step) if isinstance(step, str) and step else None
    if not isinstance(step, str) or (step and step_match is None):
        errors.append(issue("$.step", "must be empty or match Fxx-Sxx", step))
    elif step_match is not None and step_match.group(1) != feature:
        errors.append(issue("$.step", "must belong to the current feature", step))
    if step and not feature:
        errors.append(issue("$.feature", "must be set when step is set", feature))

    current_state = state.get("state")
    if current_state not in STATES:
        errors.append(issue("$.state", "unknown stable state", current_state))

    errors.extend(validate_next_actions(state.get("next_actions")))

    blockers = state.get("blockers")
    if not isinstance(blockers, list):
        errors.append(issue("$.blockers", "must be an array", blockers))
    else:
        if not all(isinstance(item, str) and item.strip() for item in blockers):
            errors.append(issue("$.blockers", "must contain only non-empty strings", blockers))
        if len(blockers) != len(set(item for item in blockers if isinstance(item, str))):
            errors.append(issue("$.blockers", "must not contain duplicates", blockers))
        if current_state == "blocked" and not blockers:
            errors.append(issue("$.blockers", "blocked state requires at least one blocker", blockers))
        if current_state != "blocked" and blockers:
            errors.append(issue("$.blockers", "must be empty unless state is blocked", blockers))

    updated_at = state.get("updated_at")
    if not isinstance(updated_at, str) or not parse_rfc3339(updated_at):
        errors.append(issue("$.updated_at", "must be an RFC 3339 timestamp with timezone", updated_at))
    if state.get("updated_by") not in {"human", "agent"}:
        errors.append(issue("$.updated_by", "must be human or agent", state.get("updated_by")))
    return errors


def state_paths(root: Path) -> Tuple[Path, Path]:
    return root / "docs/status/current.json", root / "docs/status/current.schema.json"


def roadmap_errors(errors: List[Dict]) -> List[Dict]:
    prefixed = []
    for error in errors:
        translated = copy.deepcopy(error)
        path = translated.get("path", "$")
        if not path.startswith("$roadmap"):
            translated["path"] = "$roadmap" + path[1:]
        prefixed.append(translated)
    return prefixed


def expected_next_actions(state: Dict, roadmap: Dict) -> Tuple[Optional[Dict], List[Dict]]:
    if state.get("state") != "completed":
        return {}, []
    derived = project_roadmap.derive_next_actions(
        roadmap, state.get("version"), state.get("feature"), state.get("step")
    )
    if derived is None:
        return None, [
            issue(
                "$roadmap.routes",
                "completed scope has no reviewed roadmap route",
                {
                    "version": state.get("version"),
                    "feature": state.get("feature"),
                    "step": state.get("step"),
                },
            )
        ]
    return derived, []


def validate_roadmap_consistency(state: Dict, roadmap: Dict) -> List[Dict]:
    expected, errors = expected_next_actions(state, roadmap)
    if errors:
        return errors
    if state.get("next_actions") != expected:
        return [
            issue(
                "$.next_actions",
                "must equal actions derived from the reviewed roadmap",
                {"expected": expected, "actual": state.get("next_actions")},
            )
        ]
    return []


def validate_repository_state(root: Path) -> Tuple[Optional[Dict], List[Dict]]:
    state_path, schema_path = state_paths(root)
    try:
        schema = load_json(schema_path)
        state = load_json(state_path)
    except ProjectStateError as error:
        return None, [issue("$", str(error))]
    errors = validate_state_value(state, schema)
    roadmap, loaded_errors = project_roadmap.load_validated_roadmap(root, validate_next_actions)
    errors.extend(roadmap_errors(loaded_errors))
    if roadmap is not None and not loaded_errors:
        errors.extend(validate_roadmap_consistency(state, roadmap))
    return state, errors


def reviewed_status(path: Path) -> Tuple[bool, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        return False, str(error)
    if not lines or lines[0].strip() != "---":
        return False, "missing frontmatter"
    try:
        end = lines.index("---", 1)
    except ValueError:
        return False, "unterminated frontmatter"
    matches = []
    pattern = re.compile(r"^review_status:\s*['\"]?([^'\"\s]+)['\"]?\s*$")
    for line in lines[1:end]:
        match = pattern.fullmatch(line.strip())
        if match:
            matches.append(match.group(1))
    if len(matches) != 1:
        return False, "frontmatter must contain exactly one review_status"
    if matches[0] != "reviewed":
        return False, "review_status is {}".format(matches[0])
    return True, "reviewed"


def validate_target_gate(root: Path, current: Dict, candidate: Dict, args) -> List[Dict]:
    errors = []
    target = candidate["state"]
    source = current["state"]

    if target == "ready_for_implementation" and source in {
        "awaiting_review",
        "blocked",
        "completed",
    }:
        if not args.reviewed_doc:
            errors.append(issue("$gate.reviewed_doc", "at least one --reviewed-doc is required"))
        for index, relative in enumerate(args.reviewed_doc or []):
            gate_path = "$gate.reviewed_doc[{}]".format(index)
            try:
                path = resolve_inside(root, relative, "--reviewed-doc")
            except ProjectStateError as error:
                errors.append(issue(gate_path, str(error), relative))
                continue
            if not path.is_file():
                errors.append(issue(gate_path, "reviewed document does not exist", relative))
                continue
            valid, reason = reviewed_status(path)
            if not valid:
                errors.append(issue(gate_path, reason, relative))

    if target == "awaiting_acceptance" and not args.verification_passed:
        errors.append(issue("$gate.verification_passed", "--verification-passed is required"))
    if target == "completed":
        if not args.human_confirmed:
            errors.append(issue("$gate.human_confirmed", "--human-confirmed is required"))
        if candidate["updated_by"] != "human":
            errors.append(issue("$.updated_by", "completed requires updated_by=human"))
    return errors


def atomic_write_json(path: Path, value: Dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = path.stat().st_mode & 0o777 if path.exists() else 0o644
    descriptor, temporary_name = tempfile.mkstemp(prefix=".current-", dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(value, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary_name, mode)
        os.replace(temporary_name, str(path))
    except Exception:
        try:
            os.unlink(temporary_name)
        except OSError:
            pass
        raise


def command_validate(args) -> int:
    root = Path(args.root).resolve()
    state, errors = validate_repository_state(root)
    if errors:
        emit({"status": "error", "errors": errors}, stream=sys.stderr)
        return 1
    emit({"status": "ok", "state": state})
    return 0


def command_transition(args) -> int:
    root = Path(args.root).resolve()
    state_path, schema_path = state_paths(root)
    try:
        schema = load_json(schema_path)
        current = load_json(state_path)
    except ProjectStateError as error:
        emit({"status": "error", "errors": [issue("$", str(error))]}, stream=sys.stderr)
        return 2

    current_errors = validate_state_value(current, schema)
    roadmap, loaded_errors = project_roadmap.load_validated_roadmap(root, validate_next_actions)
    current_errors.extend(roadmap_errors(loaded_errors))
    if roadmap is not None and not loaded_errors:
        current_errors.extend(validate_roadmap_consistency(current, roadmap))
    if current_errors:
        emit({"status": "error", "errors": current_errors}, stream=sys.stderr)
        return 2

    target = args.target
    if target not in ALLOWED_TRANSITIONS[current["state"]]:
        emit(
            {
                "status": "rejected",
                "current_state": current["state"],
                "target_state": target,
                "errors": [issue("$.state", "illegal transition", target)],
            },
            stream=sys.stderr,
        )
        return 3

    candidate = copy.deepcopy(current)
    candidate["state"] = target
    candidate["updated_by"] = args.updated_by
    candidate["updated_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
    for key in ("version", "feature", "step"):
        value = getattr(args, key)
        if value is not None:
            candidate[key] = value

    candidate["next_actions"] = {}
    if target == "completed":
        derived, derivation_errors = expected_next_actions(candidate, roadmap)
        if derivation_errors:
            emit(
                {
                    "status": "rejected",
                    "current_state": current["state"],
                    "target_state": target,
                    "errors": derivation_errors,
                },
                stream=sys.stderr,
            )
            return 3
        candidate["next_actions"] = derived

    candidate["blockers"] = list(args.blocker) if target == "blocked" else []
    errors = validate_state_value(candidate, schema)
    errors.extend(validate_target_gate(root, current, candidate, args))
    if errors:
        emit(
            {
                "status": "rejected",
                "current_state": current["state"],
                "target_state": target,
                "errors": errors,
            },
            stream=sys.stderr,
        )
        return 3

    changes = {
        key: {"before": current.get(key), "after": candidate.get(key)}
        for key in sorted(STATE_KEYS)
        if current.get(key) != candidate.get(key)
    }
    if args.dry_run:
        emit(
            {
                "status": "dry_run",
                "current_state": current["state"],
                "target_state": target,
                "changes": changes,
            }
        )
        return 0

    try:
        atomic_write_json(state_path, candidate)
    except OSError as error:
        emit({"status": "error", "message": str(error)}, stream=sys.stderr)
        return 4
    emit(
        {
            "status": "updated",
            "current_state": current["state"],
            "target_state": target,
            "changes": changes,
        }
    )
    return 0


def command_advance_scope(args) -> int:
    root = Path(args.root).resolve()
    state_path, schema_path = state_paths(root)
    try:
        schema = load_json(schema_path)
        current = load_json(state_path)
    except ProjectStateError as error:
        emit({"status": "error", "errors": [issue("$", str(error))]}, stream=sys.stderr)
        return 2

    current_errors = validate_state_value(current, schema)
    roadmap, loaded_errors = project_roadmap.load_validated_roadmap(root, validate_next_actions)
    current_errors.extend(roadmap_errors(loaded_errors))
    if roadmap is not None and not loaded_errors:
        current_errors.extend(validate_roadmap_consistency(current, roadmap))
    if current_errors:
        emit({"status": "error", "errors": current_errors}, stream=sys.stderr)
        return 2

    target = args.target
    errors = []
    if current["state"] != "completed":
        errors.append(issue("$.state", "scope advancement requires completed state", current["state"]))
    if not args.human_confirmed:
        errors.append(issue("$gate.human_confirmed", "--human-confirmed is required"))
    if args.updated_by != "human":
        errors.append(issue("$.updated_by", "scope advancement requires updated_by=human"))

    current_scope = (current["version"], current["feature"], current["step"])
    target_scope = (args.version, args.feature, args.step)
    if target_scope == current_scope:
        errors.append(issue("$scope", "scope advancement must select a new scope", list(target_scope)))

    allowed_scopes = {
        (current["version"], feature, action if isinstance(action, str) else action.get("step"))
        for feature, actions in current.get("next_actions", {}).items()
        for action in actions
    }
    if target_scope not in allowed_scopes:
        errors.append(
            issue(
                "$scope",
                "scope must be selected from reviewed roadmap next_actions",
                list(target_scope),
            )
        )

    candidate = copy.deepcopy(current)
    candidate.update(
        {
            "version": args.version,
            "feature": args.feature,
            "step": args.step,
            "state": target,
            "updated_by": args.updated_by,
            "updated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
            "blockers": list(args.blocker) if target == "blocked" else [],
            "next_actions": {},
        }
    )

    errors.extend(validate_state_value(candidate, schema))
    errors.extend(validate_target_gate(root, current, candidate, args))
    if errors:
        emit(
            {
                "status": "rejected",
                "current_state": current["state"],
                "target_state": target,
                "errors": errors,
            },
            stream=sys.stderr,
        )
        return 3

    changes = {
        key: {"before": current.get(key), "after": candidate.get(key)}
        for key in sorted(STATE_KEYS)
        if current.get(key) != candidate.get(key)
    }
    if args.dry_run:
        emit(
            {
                "status": "dry_run",
                "current_state": current["state"],
                "target_state": target,
                "changes": changes,
            }
        )
        return 0

    try:
        atomic_write_json(state_path, candidate)
    except OSError as error:
        emit({"status": "error", "message": str(error)}, stream=sys.stderr)
        return 4
    emit(
        {
            "status": "advanced",
            "current_state": current["state"],
            "target_state": target,
            "changes": changes,
        }
    )
    return 0


def command_reconcile_roadmap(args) -> int:
    root = Path(args.root).resolve()
    state_path, schema_path = state_paths(root)
    try:
        schema = load_json(schema_path)
        current = load_json(state_path)
    except ProjectStateError as error:
        emit({"status": "error", "errors": [issue("$", str(error))]}, stream=sys.stderr)
        return 2

    errors = validate_state_value(current, schema)
    roadmap, loaded_errors = project_roadmap.load_validated_roadmap(root, validate_next_actions)
    errors.extend(roadmap_errors(loaded_errors))
    expected = None
    if roadmap is not None and not loaded_errors:
        expected, derivation_errors = expected_next_actions(current, roadmap)
        errors.extend(derivation_errors)
    if errors:
        emit({"status": "error", "errors": errors}, stream=sys.stderr)
        return 2

    if current["next_actions"] == expected:
        emit({"status": "unchanged", "state": current})
        return 0

    candidate = copy.deepcopy(current)
    candidate["next_actions"] = expected
    candidate["updated_by"] = args.updated_by
    candidate["updated_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
    changes = {
        key: {"before": current.get(key), "after": candidate.get(key)}
        for key in sorted(STATE_KEYS)
        if current.get(key) != candidate.get(key)
    }
    if args.dry_run:
        emit({"status": "dry_run", "changes": changes})
        return 0
    try:
        atomic_write_json(state_path, candidate)
    except OSError as error:
        emit({"status": "error", "message": str(error)}, stream=sys.stderr)
        return 4
    emit({"status": "reconciled", "changes": changes})
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Validate and update UGDR project state.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_parser = subparsers.add_parser("validate", help="Validate current project state.")
    validate_parser.add_argument("--root", default=".", help="Repository root.")
    validate_parser.set_defaults(handler=command_validate)

    transition_parser = subparsers.add_parser("transition", help="Apply a guarded state transition.")
    transition_parser.add_argument("--root", default=".", help="Repository root.")
    transition_parser.add_argument("--to", dest="target", choices=sorted(STATES), required=True)
    transition_parser.add_argument("--updated-by", choices=("human", "agent"), required=True)
    transition_parser.add_argument("--version")
    transition_parser.add_argument("--feature")
    transition_parser.add_argument("--step")
    transition_parser.add_argument("--blocker", action="append", default=[])
    transition_parser.add_argument("--reviewed-doc", action="append", default=[])
    transition_parser.add_argument("--verification-passed", action="store_true")
    transition_parser.add_argument("--human-confirmed", action="store_true")
    transition_parser.add_argument("--dry-run", action="store_true")
    transition_parser.set_defaults(handler=command_transition)

    advance_parser = subparsers.add_parser(
        "advance-scope", help="Start an explicitly confirmed scope after a completed scope."
    )
    advance_parser.add_argument("--root", default=".", help="Repository root.")
    advance_parser.add_argument(
        "--to", dest="target", choices=sorted(STATES - {"completed"}), required=True
    )
    advance_parser.add_argument("--updated-by", choices=("human", "agent"), required=True)
    advance_parser.add_argument("--version", required=True)
    advance_parser.add_argument("--feature", required=True)
    advance_parser.add_argument("--step", required=True)
    advance_parser.add_argument("--blocker", action="append", default=[])
    advance_parser.add_argument("--reviewed-doc", action="append", default=[])
    advance_parser.add_argument("--verification-passed", action="store_true")
    advance_parser.add_argument("--human-confirmed", action="store_true")
    advance_parser.add_argument("--dry-run", action="store_true")
    advance_parser.set_defaults(handler=command_advance_scope)

    reconcile_parser = subparsers.add_parser(
        "reconcile-roadmap", help="Derive current next_actions from the reviewed roadmap."
    )
    reconcile_parser.add_argument("--root", default=".", help="Repository root.")
    reconcile_parser.add_argument("--updated-by", choices=("human", "agent"), required=True)
    reconcile_parser.add_argument("--dry-run", action="store_true")
    reconcile_parser.set_defaults(handler=command_reconcile_roadmap)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())

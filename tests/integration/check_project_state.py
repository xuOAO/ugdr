#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(python: str, script: Path, root: Path, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [python, str(script)] + list(arguments) + ["--root", str(root)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def fail(message: str, process: subprocess.CompletedProcess = None) -> None:
    print("project-state integration: FAIL", file=sys.stderr)
    print("- {}".format(message), file=sys.stderr)
    if process is not None:
        print("- exit: {}".format(process.returncode), file=sys.stderr)
        if process.stdout:
            print("- stdout: {}".format(process.stdout.strip()), file=sys.stderr)
        if process.stderr:
            print("- stderr: {}".format(process.stderr.strip()), file=sys.stderr)
    raise SystemExit(1)


def expect_code(process: subprocess.CompletedProcess, expected: int, message: str) -> None:
    if process.returncode != expected:
        fail(message, process)


def state_value(state: str) -> dict:
    return {
        "schema_version": 1,
        "version": "v1",
        "feature": "F01",
        "step": "F01-S02",
        "state": state,
        "next_actions": {"F01": ["F01-S02", "F01-S03"]},
        "blockers": ["external dependency"] if state == "blocked" else [],
        "updated_at": "2026-07-19T20:00:00+08:00",
        "updated_by": "agent",
    }


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def assert_unchanged(path: Path, before: bytes, message: str) -> None:
    if path.read_bytes() != before:
        fail(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", required=True)
    parser.add_argument("--script", type=Path, required=True)
    parser.add_argument("--schema", type=Path, required=True)
    parser.add_argument("--reviewed-doc", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="ugdr-project-state-") as temporary:
        root = Path(temporary)
        state_path = root / "docs/status/current.json"
        schema_path = root / "docs/status/current.schema.json"
        reviewed_path = root / "docs/reviewed.md"
        schema_path.parent.mkdir(parents=True)
        shutil.copyfile(str(args.schema), str(schema_path))
        shutil.copyfile(str(args.reviewed_doc), str(reviewed_path))

        write_json(state_path, state_value("awaiting_review"))
        process = run(args.python, args.script, root, "validate")
        expect_code(process, 0, "valid state was rejected")

        before = state_path.read_bytes()
        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "ready_for_implementation",
            "--updated-by",
            "agent",
            "--reviewed-doc",
            "docs/reviewed.md",
            "--dry-run",
        )
        expect_code(process, 0, "valid dry-run was rejected")
        assert_unchanged(state_path, before, "dry-run modified current.json")

        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "awaiting_acceptance",
            "--updated-by",
            "agent",
            "--verification-passed",
        )
        expect_code(process, 3, "illegal transition did not return exit 3")
        if '"current_state": "awaiting_review"' not in process.stderr or '"target_state": "awaiting_acceptance"' not in process.stderr:
            fail("illegal transition diagnostic omitted current or target state", process)
        assert_unchanged(state_path, before, "illegal transition modified current.json")

        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "ready_for_implementation",
            "--updated-by",
            "agent",
        )
        expect_code(process, 3, "missing review gate was accepted")
        assert_unchanged(state_path, before, "failed review gate modified current.json")

        inode_before = state_path.stat().st_ino
        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "ready_for_implementation",
            "--updated-by",
            "agent",
            "--reviewed-doc",
            "docs/reviewed.md",
        )
        expect_code(process, 0, "reviewed transition was rejected")
        if state_path.stat().st_ino == inode_before:
            fail("successful transition did not atomically replace current.json")
        if json.loads(state_path.read_text(encoding="utf-8"))["state"] != "ready_for_implementation":
            fail("successful review transition wrote the wrong state")

        before = state_path.read_bytes()
        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "awaiting_acceptance",
            "--updated-by",
            "agent",
        )
        expect_code(process, 3, "missing verification gate was accepted")
        assert_unchanged(state_path, before, "failed verification gate modified current.json")

        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "awaiting_acceptance",
            "--updated-by",
            "agent",
            "--verification-passed",
        )
        expect_code(process, 0, "verified transition was rejected")

        before = state_path.read_bytes()
        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "completed",
            "--updated-by",
            "agent",
            "--human-confirmed",
        )
        expect_code(process, 3, "agent was allowed to complete the state")
        assert_unchanged(state_path, before, "failed human gate modified current.json")

        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "completed",
            "--updated-by",
            "human",
            "--human-confirmed",
        )
        expect_code(process, 0, "human-confirmed completion was rejected")
        before = state_path.read_bytes()
        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "ready_for_implementation",
            "--updated-by",
            "human",
            "--reviewed-doc",
            "docs/reviewed.md",
        )
        expect_code(process, 3, "completed state was reopened")
        assert_unchanged(state_path, before, "terminal transition modified current.json")

        write_json(state_path, state_value("ready_for_implementation"))
        before = state_path.read_bytes()
        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "blocked",
            "--updated-by",
            "agent",
        )
        expect_code(process, 3, "blocked transition without blocker was accepted")
        assert_unchanged(state_path, before, "invalid blocked transition modified current.json")

        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "blocked",
            "--updated-by",
            "agent",
            "--blocker",
            "missing dependency",
        )
        expect_code(process, 0, "blocked transition with blocker was rejected")
        before = state_path.read_bytes()
        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "ready_for_implementation",
            "--updated-by",
            "agent",
        )
        expect_code(process, 3, "blocked recovery bypassed the target gate")
        assert_unchanged(state_path, before, "failed blocked recovery modified current.json")

        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "ready_for_implementation",
            "--updated-by",
            "agent",
            "--reviewed-doc",
            "docs/reviewed.md",
        )
        expect_code(process, 0, "valid blocked recovery was rejected")
        recovered = json.loads(state_path.read_text(encoding="utf-8"))
        if recovered["blockers"]:
            fail("blocked recovery did not clear blockers")

        bad_actions_path = root / "bad-actions.json"
        write_json(bad_actions_path, {"F01": ["F02-S01"]})
        before = state_path.read_bytes()
        process = run(
            args.python,
            args.script,
            root,
            "transition",
            "--to",
            "awaiting_review",
            "--updated-by",
            "agent",
            "--next-actions-file",
            "bad-actions.json",
        )
        expect_code(process, 3, "invalid next_actions was accepted")
        assert_unchanged(state_path, before, "invalid next_actions modified current.json")

        invalid = state_value("ready_for_implementation")
        invalid["next_actions"] = {"F01": ["F02-S01"]}
        write_json(state_path, invalid)
        process = run(args.python, args.script, root, "validate")
        expect_code(process, 1, "invalid current state was accepted")
        if "$.next_actions.F01[0]" not in process.stderr:
            fail("invalid state diagnostic omitted the field path", process)

    print("project-state integration: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

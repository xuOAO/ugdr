#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def write_json(path: Path, value: dict) -> None:
    write(path, json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def run(python: str, checker: Path, root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [python, str(checker), "--root", str(root)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def expect(process: subprocess.CompletedProcess, code: int, text: str, case: str) -> None:
    if process.returncode != code or (text and text not in process.stderr + process.stdout):
        print("project-docs fixtures: FAIL", file=sys.stderr)
        print("- case: {}".format(case), file=sys.stderr)
        print("- expected exit {} containing {!r}".format(code, text), file=sys.stderr)
        print("- actual exit {}".format(process.returncode), file=sys.stderr)
        print("- stdout: {}".format(process.stdout.strip()), file=sys.stderr)
        print("- stderr: {}".format(process.stderr.strip()), file=sys.stderr)
        raise SystemExit(1)


def valid_state() -> dict:
    return {
        "schema_version": 1,
        "version": "v1",
        "feature": "F01",
        "step": "F01-S02",
        "state": "ready_for_implementation",
        "next_actions": {},
        "blockers": [],
        "updated_at": "2026-07-19T20:00:00+08:00",
        "updated_by": "agent",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", required=True)
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--schema", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="ugdr-project-docs-") as temporary:
        root = Path(temporary)
        agents_path = root / "AGENTS.md"
        index_path = root / "docs/v1_docs/README.md"
        design_path = root / "docs/v1_docs/design.md"
        state_path = root / "docs/status/current.json"
        schema_path = root / "docs/status/current.schema.json"
        policy_path = root / "docs/governance/document-policy.json"

        valid_agents = "# Instructions\n\n## Repository Map\n\n## Project Rules\n\n## Validation\n"
        valid_index = "# Index\n\n## UGDR v1 Design Documents\n\n[Design](design.md)\n"
        write(agents_path, valid_agents)
        write(index_path, valid_index)
        write(design_path, "# Design\n")
        roadmap_source = root / "docs/v1_docs/roadmap.md"
        write(
            roadmap_source,
            "---\nreview_status: reviewed\nsource_revision: 1\n"
            "generated_body_sha256: {}\n---\n# Roadmap\n".format("c" * 64),
        )
        write_json(state_path, valid_state())
        write_json(
            root / "docs/status/roadmap.json",
            {
                "schema_version": 1,
                "reviewed_sources": [
                    {
                        "path": "docs/v1_docs/roadmap.md",
                        "revision": 1,
                        "body_sha256": "c" * 64,
                    }
                ],
                "routes": [
                    {
                        "version": "v1",
                        "feature": "F01",
                        "step": "F01-S02",
                        "source": "docs/v1_docs/roadmap.md",
                        "next_actions": {},
                    }
                ],
            },
        )
        schema_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(str(args.schema), str(schema_path))

        policy = {
            "schema_version": 1,
            "required_paths": [
                "AGENTS.md",
                "docs/status/current.json",
                "docs/status/current.schema.json",
                "docs/v1_docs/README.md",
            ],
            "max_lines": {"AGENTS.md": 8, "docs/v1_docs/README.md": 8},
            "required_headings": {
                "AGENTS.md": ["Repository Map", "Project Rules", "Validation"],
                "docs/v1_docs/README.md": ["UGDR v1 Design Documents"],
            },
            "forbidden_headings": {"AGENTS.md": ["Current Status"]},
            "link_check_paths": ["AGENTS.md", "docs/v1_docs"],
            "state": {
                "path": "docs/status/current.json",
                "schema_path": "docs/status/current.schema.json",
                "forbidden_fields": ["progress"],
            },
        }
        write_json(policy_path, policy)

        expect(run(args.python, args.checker, root), 0, "project-docs: OK", "valid fixture")

        write(index_path, valid_index.replace("design.md", "missing.md"))
        expect(run(args.python, args.checker, root), 1, "broken relative link", "broken link")
        write(index_path, valid_index)

        write(agents_path, valid_agents + "extra\nextra\n")
        expect(run(args.python, args.checker, root), 1, "exceeds maximum", "line limit")
        write(agents_path, valid_agents)

        write(agents_path, valid_agents + "\n## Current Status\n")
        expect(run(args.python, args.checker, root), 1, "forbidden heading", "responsibility boundary")
        write(agents_path, valid_agents)

        invalid_state = valid_state()
        invalid_state["next_actions"] = {"F01": ["F02-S01"]}
        write_json(state_path, invalid_state)
        expect(
            run(args.python, args.checker, root),
            1,
            "$.next_actions.F01[0]",
            "invalid next_actions",
        )

    print("project-docs fixtures: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

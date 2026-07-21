#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(python: str, checker: Path, root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [python, str(checker), "--root", str(root)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def expect(process: subprocess.CompletedProcess, code: int, marker: str, case: str) -> None:
    output = process.stdout + process.stderr
    if process.returncode != code or marker not in output:
        print("client-contract fixtures: FAIL", file=sys.stderr)
        print("- case: {}".format(case), file=sys.stderr)
        print("- expected exit {} containing {!r}".format(code, marker), file=sys.stderr)
        print("- actual exit {}".format(process.returncode), file=sys.stderr)
        print(output.strip(), file=sys.stderr)
        raise SystemExit(1)


def copy_file(repository: Path, root: Path, relative: str) -> None:
    destination = root / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(repository / relative, destination)


def create_fixture(repository: Path, root: Path) -> dict:
    manifest = json.loads((repository / "tools/client-contracts.json").read_text(encoding="utf-8"))
    paths = {
        "AGENTS.md",
        "CMakeLists.txt",
        manifest["public_header"],
        "tools/client-contracts.json",
        *manifest["required_contracts"],
        *(source["path"] for source in manifest["reviewed_sources"]),
    }
    for relative in sorted(paths):
        copy_file(repository, root, relative)
    return manifest


def write_manifest(root: Path, manifest: dict) -> None:
    (root / "tools/client-contracts.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", required=True)
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--repository", type=Path, required=True)
    args = parser.parse_args()
    repository = args.repository.resolve()

    with tempfile.TemporaryDirectory(prefix="ugdr-client-contracts-") as temporary:
        root = Path(temporary)
        manifest = create_fixture(repository, root)
        expect(run(args.python, args.checker, root), 0, "client-contracts: OK", "valid fixture")

        missing = root / "docs/contracts/object-lifecycle.md"
        saved = missing.read_text(encoding="utf-8")
        missing.unlink()
        expect(run(args.python, args.checker, root), 1, "required contract is missing", "missing contract")
        missing.write_text(saved, encoding="utf-8")

        manifest["reviewed_sources"][0]["revision"] += 1
        write_manifest(root, manifest)
        expect(run(args.python, args.checker, root), 1, "reviewed source revision mismatch", "stale revision")
        manifest["reviewed_sources"][0]["revision"] -= 1
        write_manifest(root, manifest)

        index = root / "docs/contracts/README.md"
        saved = index.read_text(encoding="utf-8")
        index.write_text(saved.replace("(wr-wc-semantics.md)", "(not-the-contract.md)"), encoding="utf-8")
        expect(run(args.python, args.checker, root), 1, "required contract is not indexed", "unindexed contract")
        index.write_text(saved, encoding="utf-8")

        agents = root / "AGENTS.md"
        saved = agents.read_text(encoding="utf-8")
        agents.write_text(saved.replace("tools/client-contracts.json", "tools/removed-policy.json"), encoding="utf-8")
        expect(run(args.python, args.checker, root), 1, "AGENTS.md repository map missing route", "missing AGENTS route")
        agents.write_text(saved, encoding="utf-8")

        removed = manifest["public_symbols"].pop()
        write_manifest(root, manifest)
        expect(run(args.python, args.checker, root), 1, "public symbol inventory missing", "missing public symbol")
        manifest["public_symbols"].append(removed)
        write_manifest(root, manifest)

    print("client-contract fixtures: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

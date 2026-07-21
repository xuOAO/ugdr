#!/usr/bin/env python3

"""Check the reviewed UGDR Client contract inventory without mutating the tree."""

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set


EXPECTED_FIELDS = {
    "schema_version",
    "public_header",
    "library_target",
    "public_symbols",
    "reviewed_sources",
    "required_contracts",
}
PUBLIC_SYMBOL_PATTERN = re.compile(r"\b(ugdr_[a-z0-9_]+)\s*\(")
FRONT_MATTER_PATTERN = re.compile(r"^---\n(.*?)\n---(?:\n|$)", re.DOTALL)


def load_json(path: Path, errors: List[str]) -> Optional[Dict]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        errors.append("cannot read manifest {}: {}".format(path, error))
        return None
    if not isinstance(value, dict):
        errors.append("manifest must contain a JSON object")
        return None
    return value


def repository_path(root: Path, value: object, field: str, errors: List[str]) -> Optional[Path]:
    if not isinstance(value, str) or not value or Path(value).is_absolute():
        errors.append("{} must be a non-empty repository-relative path".format(field))
        return None
    path = (root / value).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        errors.append("{} escapes the repository: {}".format(field, value))
        return None
    return path


def front_matter(path: Path, errors: List[str]) -> Dict[str, str]:
    try:
        contents = path.read_text(encoding="utf-8")
    except OSError as error:
        errors.append("cannot read reviewed source {}: {}".format(path, error))
        return {}
    match = FRONT_MATTER_PATTERN.match(contents)
    if not match:
        errors.append("reviewed source has no front matter: {}".format(path))
        return {}
    values = {}
    for line in match.group(1).splitlines():
        if ":" not in line:
            continue
        key, raw_value = line.split(":", 1)
        values[key.strip()] = raw_value.strip().strip('"')
    return values


def cmake_target_exists(root: Path, target: str) -> bool:
    pattern = re.compile(r"\badd_(?:library|executable)\s*\(\s*" + re.escape(target) + r"\b")
    for path in sorted(root.rglob("CMakeLists.txt")):
        try:
            if pattern.search(path.read_text(encoding="utf-8")):
                return True
        except OSError:
            continue
    return False


def check_manifest(root: Path, manifest: Dict) -> List[str]:
    errors: List[str] = []
    fields = set(manifest)
    for field in sorted(EXPECTED_FIELDS - fields):
        errors.append("manifest missing field: {}".format(field))
    for field in sorted(fields - EXPECTED_FIELDS):
        errors.append("manifest has unknown field: {}".format(field))
    if fields != EXPECTED_FIELDS:
        return errors

    if manifest["schema_version"] != 1:
        errors.append("schema_version must be 1")

    header_path = repository_path(root, manifest["public_header"], "public_header", errors)
    header_symbols: Set[str] = set()
    if header_path is not None:
        try:
            header_symbols = set(PUBLIC_SYMBOL_PATTERN.findall(header_path.read_text(encoding="utf-8")))
        except OSError as error:
            errors.append("cannot read public_header {}: {}".format(manifest["public_header"], error))

    target = manifest["library_target"]
    if not isinstance(target, str) or not target:
        errors.append("library_target must be a non-empty string")
    elif not cmake_target_exists(root, target):
        errors.append("library target is missing from CMake: {}".format(target))

    symbols = manifest["public_symbols"]
    manifest_symbols: Set[str] = set()
    if not isinstance(symbols, list) or not all(isinstance(item, str) for item in symbols):
        errors.append("public_symbols must be an array of strings")
    else:
        manifest_symbols = set(symbols)
        if len(manifest_symbols) != len(symbols):
            errors.append("public_symbols contains duplicate entries")
        for symbol in sorted(manifest_symbols - header_symbols):
            errors.append("public symbol is not declared by the header: {}".format(symbol))
        for symbol in sorted(header_symbols - manifest_symbols):
            errors.append("public symbol inventory missing header declaration: {}".format(symbol))

    sources = manifest["reviewed_sources"]
    if not isinstance(sources, list) or not sources:
        errors.append("reviewed_sources must be a non-empty array")
    else:
        seen_sources = set()
        for index, source in enumerate(sources):
            field = "reviewed_sources[{}]".format(index)
            if not isinstance(source, dict) or set(source) != {"path", "revision"}:
                errors.append("{} must contain only path and revision".format(field))
                continue
            path_value = source["path"]
            path = repository_path(root, path_value, field + ".path", errors)
            if isinstance(path_value, str):
                if path_value in seen_sources:
                    errors.append("reviewed_sources contains duplicate path: {}".format(path_value))
                seen_sources.add(path_value)
            if path is None or not path.is_file():
                errors.append("reviewed source is missing: {}".format(path_value))
                continue
            metadata = front_matter(path, errors)
            if metadata.get("review_status") != "reviewed":
                errors.append("reviewed source is not marked reviewed: {}".format(path_value))
            try:
                actual_revision = int(metadata.get("source_revision", ""))
            except ValueError:
                actual_revision = None
            if not isinstance(source["revision"], int) or source["revision"] < 1:
                errors.append("{} revision must be a positive integer".format(path_value))
            elif actual_revision != source["revision"]:
                errors.append(
                    "reviewed source revision mismatch: {} expected {} actual {}".format(
                        path_value, source["revision"], actual_revision
                    )
                )

    contracts = manifest["required_contracts"]
    contract_paths: List[str] = []
    if not isinstance(contracts, list) or not contracts or not all(isinstance(item, str) for item in contracts):
        errors.append("required_contracts must be a non-empty array of paths")
    else:
        contract_paths = contracts
        if len(set(contracts)) != len(contracts):
            errors.append("required_contracts contains duplicate entries")
        for index, contract in enumerate(contracts):
            path = repository_path(root, contract, "required_contracts[{}]".format(index), errors)
            if path is not None and not path.is_file():
                errors.append("required contract is missing: {}".format(contract))

    index_path = root / "docs/contracts/README.md"
    alignment_path = root / "docs/contracts/libibverbs-alignment.md"
    try:
        index_contents = index_path.read_text(encoding="utf-8")
    except OSError as error:
        errors.append("cannot read contract index: {}".format(error))
        index_contents = ""
    for contract in contract_paths:
        if contract == "docs/contracts/README.md":
            continue
        if Path(contract).name not in index_contents:
            errors.append("required contract is not indexed: {}".format(contract))

    try:
        alignment_contents = alignment_path.read_text(encoding="utf-8")
    except OSError as error:
        errors.append("cannot read libibverbs alignment contract: {}".format(error))
        alignment_contents = ""
    for symbol in sorted(manifest_symbols):
        if re.search(r"(?<![a-z0-9_])" + re.escape(symbol) + r"(?![a-z0-9_])", alignment_contents) is None:
            errors.append("public symbol missing from libibverbs alignment: {}".format(symbol))

    try:
        agents_contents = (root / "AGENTS.md").read_text(encoding="utf-8")
    except OSError as error:
        errors.append("cannot read AGENTS.md: {}".format(error))
        agents_contents = ""
    for marker in ("docs/contracts/", "tools/client-contracts.json", "tools/check_client_contracts.py"):
        if marker not in agents_contents:
            errors.append("AGENTS.md repository map missing route: {}".format(marker))

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Check UGDR Client contract integration policy.")
    parser.add_argument("--root", type=Path, default=Path("."), help="Repository root.")
    parser.add_argument(
        "--manifest",
        default="tools/client-contracts.json",
        help="Manifest path relative to the repository root.",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    errors: List[str] = []
    manifest_path = repository_path(root, args.manifest, "manifest", errors)
    manifest = load_json(manifest_path, errors) if manifest_path is not None else None
    if manifest is not None:
        errors.extend(check_manifest(root, manifest))
    if errors:
        print("client-contracts: FAIL", file=sys.stderr)
        for error in errors:
            print("- {}".format(error), file=sys.stderr)
        return 1
    print("client-contracts: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

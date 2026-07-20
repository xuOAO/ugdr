#!/usr/bin/env python3

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Set
from urllib.parse import unquote, urlsplit

import project_state


class DocumentCheckError(Exception):
    pass


HEADING_RE = re.compile(r"^#{1,6}\s+(.+?)\s*#*\s*$")
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")


def load_json(path: Path) -> Dict:
    try:
        with path.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        raise DocumentCheckError("{}: {}".format(path, error)) from error
    if not isinstance(value, dict):
        raise DocumentCheckError("{}: expected a JSON object".format(path))
    return value


def read_text(path: Path, relative: str, errors: List[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        errors.append("{}: cannot read UTF-8 text: {}".format(relative, error))
        return ""


def headings(content: str) -> Set[str]:
    result = set()
    in_fence = False
    for line in content.splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = HEADING_RE.fullmatch(line)
        if match:
            result.add(match.group(1).strip())
    return result


def validate_policy(policy: Dict) -> List[str]:
    errors = []
    expected = {
        "schema_version",
        "required_paths",
        "max_lines",
        "required_headings",
        "forbidden_headings",
        "link_check_paths",
        "state",
    }
    unknown = sorted(set(policy) - expected)
    missing = sorted(expected - set(policy))
    if unknown:
        errors.append("document policy has unknown fields: {}".format(", ".join(unknown)))
    if missing:
        errors.append("document policy is missing fields: {}".format(", ".join(missing)))
    if policy.get("schema_version") != 1:
        errors.append("document policy schema_version must be 1")
    for key in ("required_paths", "link_check_paths"):
        value = policy.get(key)
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            errors.append("document policy {} must be a string array".format(key))
    for key in ("max_lines", "required_headings", "forbidden_headings", "state"):
        if not isinstance(policy.get(key), dict):
            errors.append("document policy {} must be an object".format(key))
    return errors


def markdown_files(root: Path, configured: Iterable[str], errors: List[str]) -> List[Path]:
    result = []
    seen = set()
    for relative in configured:
        path = (root / relative).resolve()
        if root != path and root not in path.parents:
            errors.append("link check path escapes repository: {}".format(relative))
            continue
        candidates = sorted(path.rglob("*.md")) if path.is_dir() else [path]
        for candidate in candidates:
            if candidate.suffix.lower() != ".md" or candidate in seen:
                continue
            seen.add(candidate)
            result.append(candidate)
    return result


def link_target(raw: str) -> str:
    value = raw.strip()
    if value.startswith("<") and ">" in value:
        return value[1 : value.index(">")]
    return value.split(None, 1)[0]


def check_links(root: Path, files: Iterable[Path], errors: List[str]) -> None:
    for path in files:
        relative = str(path.relative_to(root))
        content = read_text(path, relative, errors)
        for match in LINK_RE.finditer(content):
            raw = link_target(match.group(1))
            parsed = urlsplit(raw)
            if parsed.scheme or parsed.netloc or raw.startswith("#"):
                continue
            target_text = unquote(parsed.path)
            if not target_text:
                continue
            target = (path.parent / target_text).resolve()
            if root != target and root not in target.parents:
                errors.append("{}: relative link escapes repository: {}".format(relative, raw))
            elif not target.exists():
                errors.append("{}: broken relative link: {}".format(relative, raw))


def check_policy(root: Path, policy: Dict) -> List[str]:
    errors = []
    for relative in policy["required_paths"]:
        if not (root / relative).exists():
            errors.append("{}: required path is missing".format(relative))

    content_cache = {}
    for relative, limit in policy["max_lines"].items():
        if not isinstance(limit, int) or limit <= 0:
            errors.append("{}: maximum line count must be a positive integer".format(relative))
            continue
        path = root / relative
        content = read_text(path, relative, errors)
        content_cache[relative] = content
        actual = len(content.splitlines())
        if actual > limit:
            errors.append("{}: {} lines exceeds maximum {}".format(relative, actual, limit))

    for relative, required in policy["required_headings"].items():
        content = content_cache.get(relative)
        if content is None:
            content = read_text(root / relative, relative, errors)
            content_cache[relative] = content
        actual = headings(content)
        for name in required:
            if name not in actual:
                errors.append("{}: missing required heading: {}".format(relative, name))

    for relative, forbidden in policy["forbidden_headings"].items():
        content = content_cache.get(relative)
        if content is None:
            content = read_text(root / relative, relative, errors)
            content_cache[relative] = content
        actual = headings(content)
        for name in forbidden:
            if name in actual:
                errors.append("{}: forbidden heading: {}".format(relative, name))

    state_config = policy["state"]
    state_path = root / state_config.get("path", "")
    try:
        state_value = load_json(state_path)
    except DocumentCheckError as error:
        errors.append(str(error))
    else:
        for field in state_config.get("forbidden_fields", []):
            if field in state_value:
                errors.append("{}: forbidden state field: {}".format(state_config["path"], field))
    _, state_errors = project_state.validate_repository_state(root)
    for error in state_errors:
        errors.append(
            "{}: {}{}".format(
                error["path"],
                error["reason"],
                " ({})".format(error["value"]) if "value" in error else "",
            )
        )

    files = markdown_files(root, policy["link_check_paths"], errors)
    check_links(root, files, errors)
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check UGDR project documentation governance.")
    parser.add_argument("--root", default=".", help="Repository root.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    policy_path = root / "docs/governance/document-policy.json"
    try:
        policy = load_json(policy_path)
    except DocumentCheckError as error:
        print("project-docs: FAIL", file=sys.stderr)
        print("- {}".format(error), file=sys.stderr)
        return 1

    errors = validate_policy(policy)
    if not errors:
        errors.extend(check_policy(root, policy))
    if errors:
        print("project-docs: FAIL", file=sys.stderr)
        for error in errors:
            print("- {}".format(error), file=sys.stderr)
        return 1

    print("project-docs: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

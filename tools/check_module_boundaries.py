#!/usr/bin/env python3

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple


class BoundaryCheckError(Exception):
    pass


def load_json(path: Path) -> Dict:
    try:
        with path.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        raise BoundaryCheckError("{}: {}".format(path, error)) from error
    if not isinstance(value, dict):
        raise BoundaryCheckError("{}: expected a JSON object".format(path))
    return value


def validate_policy(policy: Dict) -> List[str]:
    errors = []
    required_keys = [
        "required_paths",
        "repository_areas",
        "production_targets",
        "allowed_edges",
        "target_metadata",
        "skeleton_document",
    ]
    for key in required_keys:
        if key not in policy:
            errors.append("policy is missing required key: {}".format(key))

    areas = policy.get("repository_areas", [])
    if not isinstance(areas, list):
        errors.append("policy repository_areas must be a list")
        areas = []
    seen_areas = set()
    for area in areas:
        if not isinstance(area, dict):
            errors.append("policy repository area must be an object")
            continue
        path = area.get("path")
        responsibility = area.get("responsibility")
        if not isinstance(path, str):
            errors.append("policy repository area is missing path")
        elif path in seen_areas:
            errors.append("policy contains duplicate repository area: {}".format(path))
        else:
            seen_areas.add(path)
        if not isinstance(responsibility, str):
            errors.append("policy repository area {} is missing responsibility".format(path))

    targets = policy.get("production_targets", [])
    metadata = policy.get("target_metadata", {})
    if not isinstance(targets, list) or not all(isinstance(item, str) for item in targets):
        errors.append("policy production_targets must be a list of strings")
        targets = []
    if len(targets) != len(set(targets)):
        errors.append("policy production_targets contains duplicates")
    if not isinstance(metadata, dict):
        errors.append("policy target_metadata must be an object")
        metadata = {}
    for target in targets:
        entry = metadata.get(target)
        if not isinstance(entry, dict):
            errors.append("policy target_metadata is missing target: {}".format(target))
            continue
        if not isinstance(entry.get("path"), str):
            errors.append("policy target {} is missing path metadata".format(target))
        if not isinstance(entry.get("responsibility"), str):
            errors.append("policy target {} is missing responsibility metadata".format(target))

    target_set = set(targets)
    seen_edges = set()
    edges = policy.get("allowed_edges", [])
    if not isinstance(edges, list):
        errors.append("policy allowed_edges must be a list")
        edges = []
    for edge in edges:
        if not isinstance(edge, dict):
            errors.append("policy edge must be an object")
            continue
        caller = edge.get("from")
        dependency = edge.get("to")
        pair = (caller, dependency)
        if caller not in target_set or dependency not in target_set:
            errors.append("policy edge references unknown target: {} -> {}".format(caller, dependency))
        if pair in seen_edges:
            errors.append("policy contains duplicate edge: {} -> {}".format(caller, dependency))
        seen_edges.add(pair)
    return errors


def check_required_paths(root: Path, required_paths: List[str]) -> List[str]:
    errors = []
    for relative_path in required_paths:
        path = root / relative_path
        if not path.exists():
            errors.append("missing required path: {}".format(relative_path))
    return errors


def find_codemodel_reply(build_dir: Path) -> Path:
    reply_dir = build_dir / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply_dir.glob("index-*.json"))
    if not indexes:
        raise BoundaryCheckError(
            "CMake File API index not found under {}; configure after creating "
            "build/.cmake/api/v1/query/codemodel-v2".format(reply_dir)
        )
    index_path = indexes[-1]
    index = load_json(index_path)

    reply = index.get("reply", {})
    if isinstance(reply, dict):
        codemodel_reply = reply.get("codemodel-v2")
        if isinstance(codemodel_reply, dict) and isinstance(codemodel_reply.get("jsonFile"), str):
            return reply_dir / codemodel_reply["jsonFile"]

    for item in index.get("objects", []):
        if (
            isinstance(item, dict)
            and item.get("kind") == "codemodel"
            and item.get("version", {}).get("major") == 2
            and isinstance(item.get("jsonFile"), str)
        ):
            return reply_dir / item["jsonFile"]
    raise BoundaryCheckError("{}: codemodel v2 reply is missing".format(index_path))


def read_cmake_codemodel(build_dir: Path) -> Dict[str, Set[str]]:
    codemodel_path = find_codemodel_reply(build_dir)
    codemodel = load_json(codemodel_path)
    configurations = codemodel.get("configurations", [])
    if not configurations:
        raise BoundaryCheckError("{}: no CMake configurations found".format(codemodel_path))

    target_refs = configurations[0].get("targets", [])
    id_to_name = {}
    for target_ref in target_refs:
        target_id = target_ref.get("id")
        target_name = target_ref.get("name")
        if isinstance(target_id, str) and isinstance(target_name, str):
            id_to_name[target_id] = target_name

    graph = {}
    reply_dir = codemodel_path.parent
    for target_ref in target_refs:
        target_name = target_ref.get("name")
        json_file = target_ref.get("jsonFile")
        if not isinstance(target_name, str) or not isinstance(json_file, str):
            continue
        target = load_json(reply_dir / json_file)
        dependencies = set()
        for dependency in target.get("dependencies", []):
            dependency_id = dependency.get("id")
            dependency_name = id_to_name.get(dependency_id)
            if dependency_name:
                dependencies.add(dependency_name)
        graph[target_name] = dependencies
    return graph


def expected_edges(policy: Dict) -> Set[Tuple[str, str]]:
    return {(edge["from"], edge["to"]) for edge in policy["allowed_edges"]}


def check_targets_and_edges(policy: Dict, graph: Dict[str, Set[str]]) -> List[str]:
    errors = []
    production_targets = set(policy["production_targets"])
    allowed = expected_edges(policy)

    for target in policy["production_targets"]:
        if target not in graph:
            errors.append("missing production target: {}".format(target))

    for caller in sorted(production_targets):
        for dependency in sorted(graph.get(caller, set())):
            if (caller, dependency) not in allowed:
                errors.append("forbidden dependency: {} -> {}".format(caller, dependency))
    return errors


def render_generated_section(policy: Dict) -> str:
    document = policy["skeleton_document"]
    tick = chr(96)
    lines = [
        document["begin_marker"],
        "## Repository areas",
        "",
        "| Path | Responsibility |",
        "|---|---|",
    ]
    for area in policy["repository_areas"]:
        lines.append(
            "| {0}{1}{0} | {2} |".format(tick, area["path"], area["responsibility"])
        )

    lines.extend(
        [
            "",
            "## Production targets",
            "",
            "| Target | Path | Responsibility |",
            "|---|---|---|",
        ]
    )
    for target in policy["production_targets"]:
        metadata = policy["target_metadata"][target]
        lines.append(
            "| {0}{1}{0} | {0}{2}{0} | {3} |".format(
                tick,
                target,
                metadata["path"],
                metadata["responsibility"],
            )
        )

    dependencies = {target: [] for target in policy["production_targets"]}
    for edge in policy["allowed_edges"]:
        dependencies[edge["from"]].append(edge["to"])

    lines.extend(
        [
            "",
            "## Allowed production dependencies",
            "",
            "| Caller | Allowed dependencies |",
            "|---|---|",
        ]
    )
    for target in policy["production_targets"]:
        values = dependencies[target]
        rendered = (
            ", ".join("{0}{1}{0}".format(tick, value) for value in values)
            if values
            else "None"
        )
        lines.append("| {0}{1}{0} | {2} |".format(tick, target, rendered))
    lines.append(document["end_marker"])
    return "\n".join(lines)


def check_skeleton_document(root: Path, policy: Dict) -> List[str]:
    document = policy["skeleton_document"]
    relative_path = document.get("path")
    begin_marker = document.get("begin_marker")
    end_marker = document.get("end_marker")
    if not all(isinstance(value, str) for value in [relative_path, begin_marker, end_marker]):
        return ["policy skeleton_document must define path, begin_marker, and end_marker"]

    path = root / relative_path
    try:
        content = path.read_text(encoding="utf-8")
    except OSError as error:
        return ["cannot read skeleton document {}: {}".format(relative_path, error)]

    begin = content.find(begin_marker)
    end = content.find(end_marker, begin + len(begin_marker))
    if begin < 0 or end < 0:
        return ["skeleton document markers are missing: {}".format(relative_path)]
    end += len(end_marker)
    actual = content[begin:end]
    expected = render_generated_section(policy)
    if actual != expected:
        return ["skeleton document generated section is out of date: {}".format(relative_path)]
    return []


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check UGDR module boundaries.")
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument("--build-dir", required=True, help="Configured CMake build directory.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    build_dir = Path(args.build_dir).resolve()
    policy_path = root / "tools" / "module-boundaries.json"

    try:
        policy = load_json(policy_path)
    except BoundaryCheckError as error:
        print("module-boundaries: FAIL", file=sys.stderr)
        print("- {}".format(error), file=sys.stderr)
        return 1

    errors = validate_policy(policy)
    if not errors:
        required_paths = policy["required_paths"] + [
            area["path"] for area in policy["repository_areas"]
        ]
        errors.extend(check_required_paths(root, required_paths))
        errors.extend(check_skeleton_document(root, policy))
        try:
            graph = read_cmake_codemodel(build_dir)
        except BoundaryCheckError as error:
            errors.append(str(error))
        else:
            errors.extend(check_targets_and_edges(policy, graph))

    if errors:
        print("module-boundaries: FAIL", file=sys.stderr)
        for error in errors:
            print("- {}".format(error), file=sys.stderr)
        return 1

    print(
        "module-boundaries: OK ({} production targets, {} allowed edges)".format(
            len(policy["production_targets"]),
            len(policy["allowed_edges"]),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

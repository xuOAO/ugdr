#!/usr/bin/env python3

import json
import re
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple


VERSION_RE = re.compile(r"^v[1-9][0-9]*$")
FEATURE_RE = re.compile(r"^F[0-9]{2}$")
STEP_RE = re.compile(r"^(F[0-9]{2})-S[0-9]{2}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
ROADMAP_FIELDS = {"schema_version", "reviewed_sources", "routes"}
SOURCE_FIELDS = {"path", "revision", "body_sha256"}
ROUTE_FIELDS = {"version", "feature", "step", "source", "next_actions"}


class RoadmapError(Exception):
    pass


def issue(path: str, reason: str, value=None) -> Dict:
    result = {"path": path, "reason": reason}
    if value is not None:
        result["value"] = value
    return result


def load_json(path: Path) -> Dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RoadmapError("{}: {}".format(path, error)) from error
    if not isinstance(value, dict):
        raise RoadmapError("{}: expected a JSON object".format(path))
    return value


def _inside(root: Path, relative: str) -> Optional[Path]:
    path = Path(relative)
    if path.is_absolute():
        return None
    resolved_root = root.resolve()
    resolved = (resolved_root / path).resolve()
    if resolved != resolved_root and resolved_root not in resolved.parents:
        return None
    return resolved


def _frontmatter(path: Path) -> Dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise RoadmapError("{}: {}".format(path, error)) from error
    if not lines or lines[0].strip() != "---":
        raise RoadmapError("{}: missing frontmatter".format(path))
    try:
        end = lines.index("---", 1)
    except ValueError as error:
        raise RoadmapError("{}: unterminated frontmatter".format(path)) from error
    values = {}
    for line in lines[1:end]:
        if ":" not in line:
            continue
        key, raw = line.split(":", 1)
        value = raw.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
            value = value[1:-1]
        values[key.strip()] = value
    return values


def scope_key(version: str, feature: str, step: str) -> Tuple[str, str, str]:
    return version, feature, step


def _action_steps(next_actions: Dict) -> List[str]:
    steps = []
    for feature in sorted(next_actions):
        for action in next_actions[feature]:
            steps.append(action if isinstance(action, str) else action.get("step"))
    return steps


def validate_roadmap(
    root: Path,
    roadmap: Dict,
    next_actions_validator: Callable[[object], List[Dict]],
) -> List[Dict]:
    errors = []
    unknown = sorted(set(roadmap) - ROADMAP_FIELDS)
    missing = sorted(ROADMAP_FIELDS - set(roadmap))
    for field in unknown:
        errors.append(issue("$.{}".format(field), "unknown field", roadmap.get(field)))
    for field in missing:
        errors.append(issue("$.{}".format(field), "required field is missing"))
    if roadmap.get("schema_version") != 1:
        errors.append(issue("$.schema_version", "must be 1", roadmap.get("schema_version")))

    sources = roadmap.get("reviewed_sources")
    source_paths = set()
    if not isinstance(sources, list) or not sources:
        errors.append(issue("$.reviewed_sources", "must be a non-empty array", sources))
        sources = []
    for index, source in enumerate(sources):
        source_path = "$.reviewed_sources[{}]".format(index)
        if not isinstance(source, dict):
            errors.append(issue(source_path, "must be an object", source))
            continue
        unknown_source = sorted(set(source) - SOURCE_FIELDS)
        missing_source = sorted(SOURCE_FIELDS - set(source))
        for field in unknown_source:
            errors.append(issue(source_path + "." + field, "unknown field", source.get(field)))
        for field in missing_source:
            errors.append(issue(source_path + "." + field, "required field is missing"))
        relative = source.get("path")
        if not isinstance(relative, str) or not relative.startswith("docs/v1_docs/"):
            errors.append(issue(source_path + ".path", "must be under docs/v1_docs", relative))
            continue
        if relative in source_paths:
            errors.append(issue(source_path + ".path", "duplicate reviewed source", relative))
        source_paths.add(relative)
        resolved = _inside(root, relative)
        if resolved is None or not resolved.is_file():
            errors.append(issue(source_path + ".path", "reviewed source does not exist", relative))
            continue
        revision = source.get("revision")
        body_sha256 = source.get("body_sha256")
        if not isinstance(revision, int) or revision <= 0:
            errors.append(issue(source_path + ".revision", "must be a positive integer", revision))
        if not isinstance(body_sha256, str) or not SHA256_RE.fullmatch(body_sha256):
            errors.append(
                issue(source_path + ".body_sha256", "must be a lowercase SHA-256", body_sha256)
            )
        try:
            frontmatter = _frontmatter(resolved)
        except RoadmapError as error:
            errors.append(issue(source_path + ".path", str(error), relative))
            continue
        if frontmatter.get("review_status") != "reviewed":
            errors.append(
                issue(source_path + ".path", "source review_status must be reviewed", relative)
            )
        if str(revision) != frontmatter.get("source_revision"):
            errors.append(
                issue(source_path + ".revision", "does not match source_revision", revision)
            )
        if body_sha256 != frontmatter.get("generated_body_sha256"):
            errors.append(
                issue(source_path + ".body_sha256", "does not match reviewed body hash", body_sha256)
            )

    routes = roadmap.get("routes")
    if not isinstance(routes, list) or not routes:
        errors.append(issue("$.routes", "must be a non-empty array", routes))
        routes = []
    route_by_scope = {}
    for index, route in enumerate(routes):
        route_path = "$.routes[{}]".format(index)
        if not isinstance(route, dict):
            errors.append(issue(route_path, "must be an object", route))
            continue
        unknown_route = sorted(set(route) - ROUTE_FIELDS)
        missing_route = sorted(ROUTE_FIELDS - set(route))
        for field in unknown_route:
            errors.append(issue(route_path + "." + field, "unknown field", route.get(field)))
        for field in missing_route:
            errors.append(issue(route_path + "." + field, "required field is missing"))
        version = route.get("version")
        feature = route.get("feature")
        step = route.get("step")
        if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
            errors.append(issue(route_path + ".version", "must match vN", version))
        if not isinstance(feature, str) or not FEATURE_RE.fullmatch(feature):
            errors.append(issue(route_path + ".feature", "must match Fxx", feature))
        step_match = STEP_RE.fullmatch(step) if isinstance(step, str) else None
        if step_match is None or step_match.group(1) != feature:
            errors.append(issue(route_path + ".step", "must belong to route feature", step))
        source = route.get("source")
        if source not in source_paths:
            errors.append(issue(route_path + ".source", "must reference reviewed_sources", source))
        next_actions = route.get("next_actions")
        for action_error in next_actions_validator(next_actions):
            action_path = action_error["path"].replace(
                "$.next_actions", route_path + ".next_actions", 1
            )
            errors.append(issue(action_path, action_error["reason"], action_error.get("value")))
        if isinstance(version, str) and isinstance(feature, str) and isinstance(step, str):
            key = scope_key(version, feature, step)
            if key in route_by_scope:
                errors.append(issue(route_path, "duplicate scope route", list(key)))
            route_by_scope[key] = route

    graph = {key: [] for key in route_by_scope}
    for key, route in route_by_scope.items():
        next_actions = route.get("next_actions")
        if not isinstance(next_actions, dict):
            continue
        for target_step in _action_steps(next_actions):
            match = STEP_RE.fullmatch(target_step) if isinstance(target_step, str) else None
            if match is None:
                continue
            target_key = scope_key(key[0], match.group(1), target_step)
            if target_key not in route_by_scope:
                errors.append(
                    issue(
                        "$.routes",
                        "next action has no roadmap route",
                        {"from": key[2], "to": target_step},
                    )
                )
            else:
                graph[key].append(target_key)

    visiting = set()
    visited = set()

    def visit(key) -> None:
        if key in visiting:
            errors.append(issue("$.routes", "roadmap contains a cycle", key[2]))
            return
        if key in visited:
            return
        visiting.add(key)
        for target in graph.get(key, []):
            visit(target)
        visiting.remove(key)
        visited.add(key)

    for key in graph:
        visit(key)
    return errors


def load_validated_roadmap(
    root: Path, next_actions_validator: Callable[[object], List[Dict]]
) -> Tuple[Optional[Dict], List[Dict]]:
    path = root / "docs/status/roadmap.json"
    try:
        roadmap = load_json(path)
    except RoadmapError as error:
        return None, [issue("$roadmap", str(error))]
    errors = validate_roadmap(root, roadmap, next_actions_validator)
    return roadmap, errors


def derive_next_actions(roadmap: Dict, version: str, feature: str, step: str) -> Optional[Dict]:
    for route in roadmap["routes"]:
        if scope_key(route["version"], route["feature"], route["step"]) == scope_key(
            version, feature, step
        ):
            return json.loads(json.dumps(route["next_actions"], ensure_ascii=False))
    return None

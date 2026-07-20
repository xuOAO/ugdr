"""Fail-safe Git and pull-request lifecycle helpers for project orchestration."""

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable, List, Optional, Sequence, Tuple

import project_state

from .result import run_argv


ProcessRunner = Callable[[Sequence[str], Path, float], subprocess.CompletedProcess]
SAFE_SLUG = re.compile(r"[^a-z0-9]+")


class GitWorkflowError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class PullRequestContext:
    number: int
    state: str
    is_draft: bool
    base: str


def action_branch_name(step: str, description: str) -> str:
    slug = SAFE_SLUG.sub("-", description.lower()).strip("-")
    if not slug:
        raise GitWorkflowError("invalid_slug", "action description has no branch-safe characters")
    return "codex/{}-{}".format(step.lower(), slug[:48])


def _run_git(
    root: Path,
    arguments: Sequence[str],
    runner: ProcessRunner,
    allowed_codes: Iterable[int] = (0,),
) -> subprocess.CompletedProcess:
    process = runner(["git"] + list(arguments), root, 30.0)
    if process.returncode not in set(allowed_codes):
        diagnostic = (process.stderr or process.stdout).strip()
        raise GitWorkflowError("git_failed", diagnostic or "git command failed")
    return process


def _occupied_branches(worktree_porcelain: str) -> dict:
    occupied = {}
    location = None
    for line in worktree_porcelain.splitlines():
        if line.startswith("worktree "):
            location = line[len("worktree ") :]
        elif line.startswith("branch refs/heads/") and location:
            occupied[line[len("branch refs/heads/") :]] = location
    return occupied


def ensure_action_branch(
    root: Path,
    step: str,
    description: str,
    runner: ProcessRunner = run_argv,
) -> str:
    target = action_branch_name(step, description)
    status = _run_git(root, ["status", "--porcelain=v1"], runner)
    if status.stdout.strip():
        raise GitWorkflowError("dirty_worktree", "worktree must be clean before changing scope")
    _run_git(root, ["fetch", "origin", "master"], runner)
    local = _run_git(root, ["rev-parse", "master"], runner).stdout.strip()
    remote = _run_git(root, ["rev-parse", "origin/master"], runner).stdout.strip()
    if local != remote:
        raise GitWorkflowError("master_not_synchronized", "local master must equal origin/master")

    worktrees = _run_git(root, ["worktree", "list", "--porcelain"], runner)
    occupied = _occupied_branches(worktrees.stdout)
    current_root = str(root.resolve())
    if target in occupied and str(Path(occupied[target]).resolve()) != current_root:
        raise GitWorkflowError(
            "branch_in_other_worktree",
            "{} is already checked out at {}".format(target, occupied[target]),
        )

    existing = _run_git(root, ["branch", "--list", target], runner).stdout.strip()
    if existing:
        _run_git(root, ["switch", target], runner)
    else:
        _run_git(root, ["switch", "-c", target, "master"], runner)
    return target


def changed_paths(root: Path, runner: ProcessRunner = run_argv) -> Tuple[str, ...]:
    process = _run_git(
        root,
        ["status", "--porcelain=v1", "--untracked-files=all"],
        runner,
    )
    paths = []
    for line in process.stdout.splitlines():
        if len(line) < 4:
            continue
        path = line[3:]
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        paths.append(path)
    return tuple(sorted(paths))


def validate_commit_scope(
    changed: Iterable[str], selected: Iterable[str], allowed_prefixes: Iterable[str]
) -> Tuple[str, ...]:
    changed_set = set(changed)
    selected_set = set(selected)
    if not selected_set:
        raise GitWorkflowError("empty_commit_scope", "no paths were selected for commit")
    if selected_set != changed_set:
        raise GitWorkflowError(
            "mixed_worktree",
            "selected paths must exactly match the enumerated worktree changes",
        )
    normalized_prefixes = [PurePosixPath(value) for value in allowed_prefixes]
    for value in selected_set:
        path = PurePosixPath(value)
        if not any(path == prefix or prefix in path.parents for prefix in normalized_prefixes):
            raise GitWorkflowError("out_of_scope_path", "{} is outside the current scope".format(value))
    return tuple(sorted(selected_set))


def validate_pr_action(
    action: str,
    existing: Optional[PullRequestContext],
    review_gate: bool = False,
    acceptance_gate: bool = False,
) -> str:
    if action == "merge":
        raise GitWorkflowError("automatic_merge_forbidden", "the workflow never merges PRs")
    if action == "create_draft":
        if existing is not None:
            raise GitWorkflowError("second_pr_forbidden", "the scope already has a PR")
        if not review_gate:
            raise GitWorkflowError("review_gate_required", "review approval is required")
        return "create_draft"
    if existing is None:
        raise GitWorkflowError("missing_pr", "the scope has no existing Draft PR")
    if existing.base != "master":
        raise GitWorkflowError("wrong_base", "the PR base must be master")
    if existing.state != "OPEN":
        raise GitWorkflowError("pr_not_open", "the PR must be open")
    if action == "update_draft":
        if not existing.is_draft:
            raise GitWorkflowError("draft_required", "implementation updates require a Draft PR")
        return "update_draft"
    if action == "mark_ready":
        if not existing.is_draft:
            raise GitWorkflowError("already_ready", "the PR is already ready")
        if not acceptance_gate:
            raise GitWorkflowError("acceptance_gate_required", "final human acceptance is required")
        return "mark_ready"
    raise GitWorkflowError("unknown_pr_action", "unknown PR action: {}".format(action))


def reconcile_merged_pr(
    root: Path,
    pull_request: PullRequestContext,
    runner: ProcessRunner = run_argv,
) -> None:
    if pull_request.base != "master" or pull_request.state != "MERGED":
        raise GitWorkflowError("pr_not_merged", "only a PR merged into master may be reconciled")
    status = _run_git(root, ["status", "--porcelain=v1"], runner)
    if status.stdout.strip():
        raise GitWorkflowError("dirty_worktree", "worktree must be clean before reconciliation")
    branch = _run_git(root, ["branch", "--show-current"], runner).stdout.strip()
    _run_git(root, ["fetch", "origin", branch], runner)
    tracking = _run_git(
        root,
        ["rev-list", "--left-right", "--count", "origin/{}...HEAD".format(branch)],
        runner,
    ).stdout.split()
    if len(tracking) != 2:
        raise GitWorkflowError("invalid_tracking", "unable to determine unpushed commits")
    behind, ahead = (int(value) for value in tracking)
    if ahead:
        raise GitWorkflowError("unpushed_commits", "current branch has unpushed commits")
    if behind:
        raise GitWorkflowError("remote_branch_ahead", "current branch is behind its remote")
    _run_git(root, ["fetch", "origin", "master"], runner)
    _run_git(root, ["switch", "master"], runner)
    _run_git(root, ["merge", "--ff-only", "origin/master"], runner)
    state, errors = project_state.validate_repository_state(root)
    if errors:
        raise GitWorkflowError("invalid_project_state", "project state is invalid after reconciliation")
    if state["state"] != "completed":
        raise GitWorkflowError("unexpected_project_state", "merged master must contain completed state")

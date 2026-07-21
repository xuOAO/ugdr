"""Formatting and repository lint orchestration for the UGDR harness."""

import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, List, Optional, Sequence, Tuple

from .build import resolve_build_dir
from .result import (
    STATUS_FAIL,
    STATUS_PASS,
    CheckResult,
    CommandResult,
    ProcessRunner,
    run_argv,
)


FORMAT_FAILURE_EXIT = 30
LINT_FAILURE_EXIT = 31
QUALITY_TIMEOUT_SECONDS = 180.0
MANAGED_SOURCE_ROOTS = ("include", "src", "apps", "tests")
FORMAT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".cu", ".cuh", ".h", ".hh", ".hpp"}
TIDY_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}

Which = Callable[[str], Optional[str]]


def managed_source_files(root: Path) -> Tuple[str, ...]:
    files = []
    for relative_root in MANAGED_SOURCE_ROOTS:
        directory = root / relative_root
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix.lower() in FORMAT_SUFFIXES:
                files.append(path.relative_to(root).as_posix())
    return tuple(sorted(files))


def _failure(check_id: str, path: str, diagnostic: str, remediation: str) -> CheckResult:
    return CheckResult(
        check_id=check_id,
        required=True,
        status=STATUS_FAIL,
        path=path,
        diagnostic=diagnostic,
        remediation=remediation,
    )


def _normalize_output(completed: subprocess.CompletedProcess, root: Path) -> str:
    output = "\n".join(
        value.strip()
        for value in (completed.stdout or "", completed.stderr or "")
        if value.strip()
    )
    return output.replace(str(root), ".")


def _process_check(
    check_id: str,
    path: str,
    argv: Sequence[str],
    root: Path,
    success_diagnostic: str,
    remediation: str,
    runner: ProcessRunner,
    timeout: float,
) -> CheckResult:
    try:
        completed = runner(argv, root, timeout)
    except subprocess.TimeoutExpired:
        return _failure(
            check_id,
            path,
            "command timed out after {} seconds".format(timeout),
            remediation,
        )
    except (OSError, TypeError) as error:
        return _failure(
            check_id,
            path,
            "command could not be executed: {}".format(error),
            remediation,
        )
    if completed.returncode == 0:
        return CheckResult(
            check_id=check_id,
            required=True,
            status=STATUS_PASS,
            path=path,
            diagnostic=success_diagnostic,
            remediation="",
        )
    diagnostic = _normalize_output(completed, root)
    if not diagnostic:
        diagnostic = "command exited with {}".format(completed.returncode)
    return _failure(check_id, path, diagnostic, remediation)


def _format_check(
    root: Path,
    check_id: str,
    check_only: bool,
    which: Which,
    runner: ProcessRunner,
    timeout: float,
) -> CheckResult:
    files = managed_source_files(root)
    if not files:
        return _failure(
            check_id,
            ",".join(MANAGED_SOURCE_ROOTS),
            "no managed C/C++/CUDA files were found",
            "Restore the repository source skeleton and retry.",
        )
    clang_format = which("clang-format")
    if not clang_format:
        return _failure(
            check_id,
            ".clang-format",
            "clang-format is missing or not executable",
            "Install or expose clang-format on PATH.",
        )
    mode = ["--dry-run", "--Werror"] if check_only else ["-i"]
    argv = [clang_format, "--style=file"] + mode + list(files)
    action = "match" if check_only else "were formatted with"
    return _process_check(
        check_id,
        ".clang-format",
        argv,
        root,
        "{} managed files {} .clang-format".format(len(files), action),
        "Run tools/ugdr format and review the resulting changes.",
        runner,
        timeout,
    )


def run_format(
    root: Path,
    check_only: bool = False,
    which: Which = shutil.which,
    runner: ProcessRunner = run_argv,
    timeout: float = QUALITY_TIMEOUT_SECONDS,
) -> CommandResult:
    check = _format_check(
        root.resolve(),
        "format.clang_format",
        check_only,
        which,
        runner,
        timeout,
    )
    exit_code = 0 if check.status == STATUS_PASS else FORMAT_FAILURE_EXIT
    return CommandResult("format", (check,), exit_code)


def _compile_database_files(root: Path, build_path: Path) -> Tuple[Tuple[str, ...], str]:
    compile_database = build_path / "compile_commands.json"
    try:
        entries = json.loads(compile_database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return (), "cannot read compile_commands.json: {}".format(error)
    if not isinstance(entries, list):
        return (), "compile_commands.json must contain an array"

    files = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            continue
        path = Path(entry["file"])
        if not path.is_absolute():
            directory_value = entry.get("directory", str(root))
            if not isinstance(directory_value, str):
                continue
            directory = Path(directory_value)
            path = directory / path
        path = path.resolve()
        try:
            relative = path.relative_to(root)
        except ValueError:
            continue
        if (
            relative.parts
            and relative.parts[0] in MANAGED_SOURCE_ROOTS
            and path.suffix.lower() in TIDY_SUFFIXES
        ):
            files.add(relative.as_posix())
    if not files:
        return (), "compile_commands.json contains no managed C/C++ translation units"
    return tuple(sorted(files)), ""


def _clang_tidy_check(
    root: Path,
    build_path: Optional[Path],
    build_relative: str,
    build_error: str,
    which: Which,
    runner: ProcessRunner,
    timeout: float,
) -> CheckResult:
    if build_error:
        return _failure(
            "lint.clang_tidy",
            "",
            build_error,
            "Choose a configured build directory below the repository root.",
        )
    clang_tidy = which("clang-tidy")
    if not clang_tidy:
        return _failure(
            "lint.clang_tidy",
            ".clang-tidy",
            "clang-tidy is missing or not executable",
            "Install or expose clang-tidy on PATH.",
        )
    files, error = _compile_database_files(root, build_path)
    if error:
        return _failure(
            "lint.clang_tidy",
            build_relative,
            error,
            "Run tools/ugdr build --build-dir {} before lint.".format(build_relative),
        )
    argv = [clang_tidy, "-p", build_relative, "--quiet"] + list(files)
    return _process_check(
        "lint.clang_tidy",
        build_relative,
        argv,
        root,
        "clang-tidy passed for {} translation units".format(len(files)),
        "Fix the reported clang-tidy diagnostics and retry.",
        runner,
        timeout,
    )


def _repository_check(
    check_id: str,
    path: str,
    argv: Sequence[str],
    root: Path,
    success_diagnostic: str,
    remediation: str,
    runner: ProcessRunner,
    timeout: float,
) -> CheckResult:
    return _process_check(
        check_id,
        path,
        argv,
        root,
        success_diagnostic,
        remediation,
        runner,
        timeout,
    )


def run_lint(
    root: Path,
    build_dir: str = "build",
    which: Which = shutil.which,
    runner: ProcessRunner = run_argv,
    timeout: float = QUALITY_TIMEOUT_SECONDS,
) -> CommandResult:
    root = root.resolve()
    build_path, build_relative, build_error = resolve_build_dir(root, build_dir)

    checks: List[CheckResult] = [
        _format_check(
            root,
            "lint.format",
            True,
            which,
            runner,
            timeout,
        ),
        _clang_tidy_check(
            root,
            build_path,
            build_relative,
            build_error,
            which,
            runner,
            timeout,
        ),
    ]

    if build_error:
        checks.append(
            _failure(
                "lint.module_boundaries",
                "",
                build_error,
                "Choose a configured build directory below the repository root.",
            )
        )
    else:
        checks.append(
            _repository_check(
                "lint.module_boundaries",
                "tools/module-boundaries.json",
                [
                    sys.executable,
                    "tools/check_module_boundaries.py",
                    "--root",
                    ".",
                    "--build-dir",
                    build_relative,
                ],
                root,
                "module dependency boundaries passed",
                "Fix the module boundary or configure the build tree and retry.",
                runner,
                timeout,
            )
        )

    checks.extend(
        [
            _repository_check(
                "lint.client_contracts",
                "tools/client-contracts.json",
                [sys.executable, "tools/check_client_contracts.py", "--root", "."],
                root,
                "Client contract integration policy passed",
                "Fix the Client contract inventory, source revisions, routes, or symbol coverage.",
                runner,
                timeout,
            ),
            _repository_check(
                "lint.project_docs",
                "docs/governance/document-policy.json",
                [sys.executable, "tools/check_project_docs.py", "--root", "."],
                root,
                "documentation governance passed",
                "Fix the reported documentation governance failures.",
                runner,
                timeout,
            ),
            _repository_check(
                "lint.skeleton",
                "docs/architecture/repository-skeleton.md",
                [
                    sys.executable,
                    "tools/check_module_boundaries.py",
                    "--root",
                    ".",
                    "--skeleton-only",
                ],
                root,
                "repository skeleton matches module-boundaries policy",
                "Regenerate or update the repository skeleton from the policy.",
                runner,
                timeout,
            ),
            _repository_check(
                "lint.project_state",
                "docs/status/current.json",
                [sys.executable, "tools/project_state.py", "validate", "--root", "."],
                root,
                "project state is valid",
                "Use project_state.py transition, advance-scope, or reconcile-roadmap to repair project state.",
                runner,
                timeout,
            ),
        ]
    )

    exit_code = 0 if all(check.status == STATUS_PASS for check in checks) else LINT_FAILURE_EXIT
    return CommandResult("lint", tuple(checks), exit_code)

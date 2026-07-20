"""Repository-local bootstrap behavior for the UGDR command."""

import shutil
from pathlib import Path
from typing import Union

from .environment import (
    COMMAND_TIMEOUT_SECONDS,
    STATUS_FAIL,
    STATUS_PASS,
    CheckResult,
    CommandResult,
    Runner,
    Which,
    default_runner,
    probe_environment,
    select_exit_code,
)


def _failure(diagnostic: str, remediation: str) -> CheckResult:
    return CheckResult(
        check_id="bootstrap.file_api",
        required=True,
        status=STATUS_FAIL,
        path="",
        diagnostic=diagnostic,
        remediation=remediation,
    )


def ensure_cmake_file_api_query(
    root: Path, build_dir: Union[str, Path]
) -> CheckResult:
    root = root.resolve()
    if not (root / "CMakeLists.txt").is_file() or not (root / "tools" / "ugdr").is_file():
        return _failure(
            "repository root is invalid",
            "Run the repository-local tools/ugdr entry from a complete checkout.",
        )

    requested = Path(build_dir)
    candidate = requested if requested.is_absolute() else root / requested
    candidate = candidate.resolve()
    try:
        relative_build_dir = candidate.relative_to(root)
    except ValueError:
        return _failure(
            "requested build directory is outside the repository",
            "Choose a build directory located below the repository root.",
        )
    if not relative_build_dir.parts:
        return _failure(
            "repository root cannot be used as the build directory",
            "Choose a dedicated build directory below the repository root.",
        )

    relative_query = (
        relative_build_dir / ".cmake" / "api" / "v1" / "query" / "codemodel-v2"
    )
    query = root / relative_query
    try:
        if query.exists():
            if not query.is_file():
                return _failure(
                    "CMake File API query path exists but is not a file",
                    "Remove the conflicting repository-local path and retry.",
                )
        else:
            query.parent.mkdir(parents=True, exist_ok=True)
            with query.open("x", encoding="utf-8"):
                pass
    except OSError as error:
        return _failure(
            "could not create CMake File API query: {}".format(error),
            "Check repository-local build directory permissions and retry.",
        )

    return CheckResult(
        check_id="bootstrap.file_api",
        required=True,
        status=STATUS_PASS,
        path=relative_query.as_posix(),
        diagnostic="CMake File API codemodel-v2 query is present",
        remediation="",
    )


def run_bootstrap(
    root: Path,
    build_dir: Union[str, Path] = "build",
    which: Which = shutil.which,
    runner: Runner = default_runner,
    timeout: float = COMMAND_TIMEOUT_SECONDS,
) -> CommandResult:
    checks = list(probe_environment(which=which, runner=runner, timeout=timeout))
    checks.append(ensure_cmake_file_api_query(root, build_dir))
    result_checks = tuple(checks)
    return CommandResult("bootstrap", result_checks, select_exit_code(result_checks))

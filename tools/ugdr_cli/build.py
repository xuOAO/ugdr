"""CMake, Ninja, CTest, and smoke orchestration for the UGDR harness."""

import json
import shutil
import subprocess
from pathlib import Path
from typing import Callable, Optional, Sequence, Tuple, Union

from .bootstrap import ensure_cmake_file_api_query
from .result import (
    STATUS_FAIL,
    STATUS_PASS,
    STATUS_SKIP,
    CheckResult,
    CommandResult,
    ProcessRunner,
    run_argv,
)


BUILD_FAILURE_EXIT = 32
TEST_FAILURE_EXIT = 33
SMOKE_FAILURE_EXIT = 34
BUILD_TIMEOUT_SECONDS = 300.0
EXPECTED_SMOKE_TESTS = (
    "project_handoff_surface",
    "ugdr_client_smoke",
    "ugdr_daemon_smoke",
)
PROTECTED_SOURCE_AREAS = {
    ".git",
    ".lark",
    "apps",
    "docs",
    "include",
    "src",
    "tests",
    "tools",
}

Which = Callable[[str], Optional[str]]


def resolve_build_dir(
    root: Path, build_dir: Union[str, Path]
) -> Tuple[Optional[Path], str, str]:
    """Resolve a repository-local build directory and reject source areas."""
    root = root.resolve()
    requested = Path(build_dir)
    candidate = requested if requested.is_absolute() else root / requested
    candidate = candidate.resolve()
    try:
        relative = candidate.relative_to(root)
    except ValueError:
        return None, "", "requested build directory is outside the repository"
    if not relative.parts:
        return None, "", "repository root cannot be used as the build directory"
    if relative.parts[0] in PROTECTED_SOURCE_AREAS:
        return None, "", "source area cannot be used as the build directory"
    if candidate.exists() and not candidate.is_dir():
        return None, "", "requested build directory exists but is not a directory"
    return candidate, relative.as_posix(), ""


def _failure(check_id: str, path: str, diagnostic: str, remediation: str) -> CheckResult:
    return CheckResult(
        check_id=check_id,
        required=True,
        status=STATUS_FAIL,
        path=path,
        diagnostic=diagnostic,
        remediation=remediation,
    )


def _skip(check_id: str, path: str, reason: str) -> CheckResult:
    return CheckResult(
        check_id=check_id,
        required=True,
        status=STATUS_SKIP,
        path=path,
        diagnostic=reason,
        remediation="Resolve the preceding failure and retry.",
    )


def _normalize_output(completed: subprocess.CompletedProcess, root: Path) -> str:
    output = "\n".join(
        value.strip()
        for value in (completed.stdout or "", completed.stderr or "")
        if value.strip()
    )
    return output.replace(str(root), ".")


def _run_process_check(
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


def _configure(
    command: str,
    root: Path,
    build_path: Path,
    build_relative: str,
    which: Which,
    runner: ProcessRunner,
    timeout: float,
) -> CheckResult:
    check_id = "{}.configure".format(command)
    cmake = which("cmake")
    if not cmake:
        return _failure(
            check_id,
            build_relative,
            "cmake is missing or not executable",
            "Install or expose cmake on PATH.",
        )
    file_api = ensure_cmake_file_api_query(root, build_path)
    if file_api.status != STATUS_PASS:
        return _failure(
            check_id,
            build_relative,
            file_api.diagnostic,
            file_api.remediation,
        )
    argv = [
        cmake,
        "-S",
        ".",
        "-B",
        build_relative,
        "-G",
        "Ninja",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    return _run_process_check(
        check_id,
        build_relative,
        argv,
        root,
        "CMake configured the Ninja build tree",
        "Fix the CMake configuration error and retry.",
        runner,
        timeout,
    )


def _invalid_build_dir(command: str, diagnostic: str) -> CheckResult:
    return _failure(
        "{}.configure".format(command),
        "",
        diagnostic,
        "Choose a dedicated build directory below the repository root.",
    )


def run_build(
    root: Path,
    build_dir: Union[str, Path] = "build",
    which: Which = shutil.which,
    runner: ProcessRunner = run_argv,
    timeout: float = BUILD_TIMEOUT_SECONDS,
) -> CommandResult:
    build_path, build_relative, error = resolve_build_dir(root, build_dir)
    if error:
        checks = (
            _invalid_build_dir("build", error),
            _skip("build.compile", "", "compile skipped because the build directory is invalid"),
        )
        return CommandResult("build", checks, BUILD_FAILURE_EXIT)

    configure = _configure(
        "build", root.resolve(), build_path, build_relative, which, runner, timeout
    )
    if configure.status != STATUS_PASS:
        checks = (
            configure,
            _skip(
                "build.compile",
                build_relative,
                "compile skipped because configuration failed",
            ),
        )
        return CommandResult("build", checks, BUILD_FAILURE_EXIT)

    cmake = which("cmake")
    compile_check = _run_process_check(
        "build.compile",
        build_relative,
        [cmake, "--build", build_relative],
        root.resolve(),
        "Ninja build completed",
        "Fix the compiler or Ninja failure and retry.",
        runner,
        timeout,
    )
    checks = (configure, compile_check)
    exit_code = 0 if compile_check.status == STATUS_PASS else BUILD_FAILURE_EXIT
    return CommandResult("build", checks, exit_code)


def run_test(
    root: Path,
    build_dir: Union[str, Path] = "build",
    which: Which = shutil.which,
    runner: ProcessRunner = run_argv,
    timeout: float = BUILD_TIMEOUT_SECONDS,
) -> CommandResult:
    root = root.resolve()
    build_path, build_relative, error = resolve_build_dir(root, build_dir)
    if error:
        checks = (
            _invalid_build_dir("test", error),
            _skip("test.build", "", "build skipped because the build directory is invalid"),
            _skip("test.ctest", "", "CTest skipped because the build directory is invalid"),
        )
        return CommandResult("test", checks, TEST_FAILURE_EXIT)

    configure = _configure(
        "test", root, build_path, build_relative, which, runner, timeout
    )
    if configure.status != STATUS_PASS:
        return CommandResult(
            "test",
            (
                configure,
                _skip("test.build", build_relative, "build skipped because configuration failed"),
                _skip("test.ctest", build_relative, "CTest skipped because configuration failed"),
            ),
            TEST_FAILURE_EXIT,
        )

    cmake = which("cmake")
    build_check = _run_process_check(
        "test.build",
        build_relative,
        [cmake, "--build", build_relative],
        root,
        "Ninja build completed before CTest",
        "Fix the build failure and retry the full test command.",
        runner,
        timeout,
    )
    if build_check.status != STATUS_PASS:
        return CommandResult(
            "test",
            (
                configure,
                build_check,
                _skip("test.ctest", build_relative, "CTest skipped because the build failed"),
            ),
            TEST_FAILURE_EXIT,
        )

    ctest = which("ctest")
    if not ctest:
        ctest_check = _failure(
            "test.ctest",
            build_relative,
            "ctest is missing or not executable",
            "Install or expose ctest on PATH.",
        )
    else:
        ctest_check = _run_process_check(
            "test.ctest",
            build_relative,
            [ctest, "--test-dir", build_relative, "--output-on-failure"],
            root,
            "full CTest suite passed",
            "Inspect the failing CTest output, fix the regression, and retry.",
            runner,
            timeout,
        )
    checks = (configure, build_check, ctest_check)
    exit_code = 0 if ctest_check.status == STATUS_PASS else TEST_FAILURE_EXIT
    return CommandResult("test", checks, exit_code)


def _smoke_selection_check(
    root: Path,
    build_relative: str,
    ctest: str,
    runner: ProcessRunner,
    timeout: float,
) -> CheckResult:
    argv = [
        ctest,
        "--test-dir",
        build_relative,
        "--show-only=json-v1",
        "--label-regex",
        "^ugdr_smoke$",
    ]
    try:
        completed = runner(argv, root, timeout)
    except (subprocess.TimeoutExpired, OSError) as error:
        return _failure(
            "smoke.selection",
            build_relative,
            "could not enumerate smoke tests: {}".format(error),
            "Fix CTest discovery and retry.",
        )
    if completed.returncode != 0:
        diagnostic = _normalize_output(completed, root) or "CTest discovery failed"
        return _failure(
            "smoke.selection",
            build_relative,
            diagnostic,
            "Fix CTest discovery and retry.",
        )
    try:
        payload = json.loads(completed.stdout)
        selected = tuple(sorted(test["name"] for test in payload.get("tests", [])))
    except (AttributeError, json.JSONDecodeError, KeyError, TypeError) as error:
        return _failure(
            "smoke.selection",
            build_relative,
            "CTest discovery output is invalid: {}".format(error),
            "Use a CTest version that supports --show-only=json-v1.",
        )
    expected = tuple(sorted(EXPECTED_SMOKE_TESTS))
    if selected != expected:
        return _failure(
            "smoke.selection",
            build_relative,
            "smoke label selected [{}], expected [{}]".format(
                ", ".join(selected), ", ".join(expected)
            ),
            "Apply the ugdr_smoke label only to the handoff, client, and daemon checks.",
        )
    return CheckResult(
        check_id="smoke.selection",
        required=True,
        status=STATUS_PASS,
        path=build_relative,
        diagnostic="smoke label selects only handoff, client, and daemon checks",
        remediation="",
        details={"selected_tests": list(expected)},
    )


def run_smoke(
    root: Path,
    build_dir: Union[str, Path] = "build",
    which: Which = shutil.which,
    runner: ProcessRunner = run_argv,
    timeout: float = BUILD_TIMEOUT_SECONDS,
) -> CommandResult:
    root = root.resolve()
    build_path, build_relative, error = resolve_build_dir(root, build_dir)
    if error:
        checks = (
            _invalid_build_dir("smoke", error),
            _skip("smoke.build", "", "build skipped because the build directory is invalid"),
            _skip("smoke.selection", "", "selection skipped because the build directory is invalid"),
            _skip("smoke.ctest", "", "CTest skipped because the build directory is invalid"),
        )
        return CommandResult("smoke", checks, SMOKE_FAILURE_EXIT)

    configure = _configure(
        "smoke", root, build_path, build_relative, which, runner, timeout
    )
    if configure.status != STATUS_PASS:
        return CommandResult(
            "smoke",
            (
                configure,
                _skip("smoke.build", build_relative, "build skipped because configuration failed"),
                _skip("smoke.selection", build_relative, "selection skipped because configuration failed"),
                _skip("smoke.ctest", build_relative, "CTest skipped because configuration failed"),
            ),
            SMOKE_FAILURE_EXIT,
        )

    cmake = which("cmake")
    build_check = _run_process_check(
        "smoke.build",
        build_relative,
        [
            cmake,
            "--build",
            build_relative,
            "--target",
            "ugdr_client",
            "ugdr_daemon",
        ],
        root,
        "required client and daemon targets were built",
        "Fix the client or daemon target build and retry.",
        runner,
        timeout,
    )
    if build_check.status != STATUS_PASS:
        return CommandResult(
            "smoke",
            (
                configure,
                build_check,
                _skip("smoke.selection", build_relative, "selection skipped because the build failed"),
                _skip("smoke.ctest", build_relative, "CTest skipped because the build failed"),
            ),
            SMOKE_FAILURE_EXIT,
        )

    ctest = which("ctest")
    if not ctest:
        selection = _failure(
            "smoke.selection",
            build_relative,
            "ctest is missing or not executable",
            "Install or expose ctest on PATH.",
        )
    else:
        selection = _smoke_selection_check(
            root, build_relative, ctest, runner, timeout
        )
    if selection.status != STATUS_PASS:
        return CommandResult(
            "smoke",
            (
                configure,
                build_check,
                selection,
                _skip("smoke.ctest", build_relative, "CTest skipped because smoke selection is invalid"),
            ),
            SMOKE_FAILURE_EXIT,
        )

    ctest_check = _run_process_check(
        "smoke.ctest",
        build_relative,
        [
            ctest,
            "--test-dir",
            build_relative,
            "--output-on-failure",
            "--label-regex",
            "^ugdr_smoke$",
        ],
        root,
        "minimal handoff, client, and daemon smoke checks passed",
        "Inspect the failing smoke check and retry.",
        runner,
        timeout,
    )
    checks = (configure, build_check, selection, ctest_check)
    exit_code = 0 if ctest_check.status == STATUS_PASS else SMOKE_FAILURE_EXIT
    return CommandResult("smoke", checks, exit_code)

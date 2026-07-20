"""Deterministic environment probes used by the UGDR command."""

import re
import shutil
import subprocess
from typing import Callable, List, Optional, Sequence, Tuple

from .result import (
    STATUS_FAIL,
    STATUS_PASS,
    CheckResult,
    CommandResult,
    render_human,
    render_json,
)


REQUIRED_BASE_COMMANDS = (
    "python3",
    "cmake",
    "ninja",
    "gcc",
    "g++",
    "clang",
    "clang-format",
    "clang-tidy",
)
CUDA_VERSION_LOWER = (12, 0)
CUDA_VERSION_UPPER = (13, 0)
CUDA_REQUIRED_RANGE = "12 < version < 13"
COMMAND_TIMEOUT_SECONDS = 5.0

Which = Callable[[str], Optional[str]]
Runner = Callable[[Sequence[str], float], subprocess.CompletedProcess]


def default_runner(command: Sequence[str], timeout: float) -> subprocess.CompletedProcess:
    return subprocess.run(
        list(command),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
        shell=False,
    )


def _base_command_check(name: str, which: Which) -> CheckResult:
    path = which(name)
    if path:
        return CheckResult(
            check_id="base.{}".format(name),
            required=True,
            status=STATUS_PASS,
            path=path,
            diagnostic="{} is available".format(name),
            remediation="",
        )
    return CheckResult(
        check_id="base.{}".format(name),
        required=True,
        status=STATUS_FAIL,
        path="",
        diagnostic="{} is missing or not executable".format(name),
        remediation="Install or expose {} on PATH.".format(name),
    )


def _invoke(
    runner: Runner, command: Sequence[str], timeout: float
) -> Tuple[Optional[subprocess.CompletedProcess], str]:
    try:
        return runner(command, timeout), ""
    except subprocess.TimeoutExpired:
        return None, "command timed out after {} seconds".format(timeout)
    except OSError as error:
        return None, "command could not be executed: {}".format(error)


_CUDA_PATTERNS = (
    re.compile(r"\brelease\s+(\d+)(?:\.(\d+))?(?:\.(\d+))?", re.IGNORECASE),
    re.compile(r"\bV(\d+)(?:\.(\d+))?(?:\.(\d+))?\b"),
)


def parse_cuda_version(output: str) -> Optional[Tuple[str, Tuple[int, int]]]:
    for pattern in _CUDA_PATTERNS:
        match = pattern.search(output)
        if not match:
            continue
        raw_parts = [part for part in match.groups() if part is not None]
        numeric = [int(part) for part in raw_parts]
        while len(numeric) < 2:
            numeric.append(0)
        return ".".join(raw_parts), tuple(numeric[:2])
    return None


def _cuda_check(which: Which, runner: Runner, timeout: float) -> CheckResult:
    path = which("nvcc")
    details = {
        "observed_version": "",
        "required_range": CUDA_REQUIRED_RANGE,
    }
    if not path:
        return CheckResult(
            check_id="cuda.toolkit",
            required=True,
            status=STATUS_FAIL,
            path="",
            diagnostic="nvcc is missing or not executable",
            remediation="Install or expose a CUDA Toolkit satisfying {}.".format(
                CUDA_REQUIRED_RANGE
            ),
            details=details,
        )

    completed, invocation_error = _invoke(runner, [path, "--version"], timeout)
    if completed is None:
        return CheckResult(
            check_id="cuda.toolkit",
            required=True,
            status=STATUS_FAIL,
            path=path,
            diagnostic="nvcc version probe failed: {}".format(invocation_error),
            remediation="Make nvcc executable and verify the CUDA Toolkit installation.",
            details=details,
        )
    if completed.returncode != 0:
        return CheckResult(
            check_id="cuda.toolkit",
            required=True,
            status=STATUS_FAIL,
            path=path,
            diagnostic="nvcc --version exited with {}".format(completed.returncode),
            remediation="Repair the CUDA Toolkit installation and retry.",
            details=details,
        )

    parsed = parse_cuda_version("{}\n{}".format(completed.stdout, completed.stderr))
    if parsed is None:
        return CheckResult(
            check_id="cuda.toolkit",
            required=True,
            status=STATUS_FAIL,
            path=path,
            diagnostic="CUDA version could not be parsed from nvcc output",
            remediation="Use an nvcc installation that reports its CUDA release.",
            details=details,
        )

    observed, comparable = parsed
    details["observed_version"] = observed
    if CUDA_VERSION_LOWER < comparable < CUDA_VERSION_UPPER:
        return CheckResult(
            check_id="cuda.toolkit",
            required=True,
            status=STATUS_PASS,
            path=path,
            diagnostic="CUDA {} satisfies {}".format(observed, CUDA_REQUIRED_RANGE),
            remediation="",
            details=details,
        )
    return CheckResult(
        check_id="cuda.toolkit",
        required=True,
        status=STATUS_FAIL,
        path=path,
        diagnostic="CUDA {} does not satisfy {}".format(observed, CUDA_REQUIRED_RANGE),
        remediation="Use a CUDA Toolkit satisfying {}.".format(CUDA_REQUIRED_RANGE),
        details=details,
    )


def _gpu_check(which: Which, runner: Runner, timeout: float) -> CheckResult:
    path = which("nvidia-smi")
    if not path:
        return CheckResult(
            check_id="gpu.nvidia",
            required=True,
            status=STATUS_FAIL,
            path="",
            diagnostic="nvidia-smi is missing or not executable",
            remediation="Install a working NVIDIA driver and expose nvidia-smi on PATH.",
        )

    command = [path, "--query-gpu=index", "--format=csv,noheader,nounits"]
    completed, invocation_error = _invoke(runner, command, timeout)
    if completed is None:
        return CheckResult(
            check_id="gpu.nvidia",
            required=True,
            status=STATUS_FAIL,
            path=path,
            diagnostic="GPU probe failed: {}".format(invocation_error),
            remediation="Verify NVIDIA driver communication and GPU availability.",
        )
    gpu_rows = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    valid_gpu_rows = gpu_rows and all(re.fullmatch(r"\d+", row) for row in gpu_rows)
    if completed.returncode == 0 and valid_gpu_rows:
        return CheckResult(
            check_id="gpu.nvidia",
            required=True,
            status=STATUS_PASS,
            path=path,
            diagnostic="NVIDIA driver communication succeeded and {} GPU(s) were found".format(
                len(gpu_rows)
            ),
            remediation="",
        )
    return CheckResult(
        check_id="gpu.nvidia",
        required=True,
        status=STATUS_FAIL,
        path=path,
        diagnostic="NVIDIA driver communication failed or no GPU was found",
        remediation="Verify the NVIDIA driver and ensure at least one GPU is visible.",
    )


def probe_environment(
    which: Which = shutil.which,
    runner: Runner = default_runner,
    timeout: float = COMMAND_TIMEOUT_SECONDS,
) -> Tuple[CheckResult, ...]:
    checks: List[CheckResult] = [
        _base_command_check(name, which) for name in REQUIRED_BASE_COMMANDS
    ]
    checks.append(_cuda_check(which, runner, timeout))
    checks.append(_gpu_check(which, runner, timeout))
    return tuple(checks)


def select_exit_code(checks: Sequence[CheckResult]) -> int:
    failures = {check.check_id for check in checks if check.status == STATUS_FAIL}
    if "bootstrap.file_api" in failures:
        return 20
    if any(check_id.startswith("base.") for check_id in failures):
        return 10
    if "cuda.toolkit" in failures:
        return 12
    if "gpu.nvidia" in failures:
        return 13
    return 0


def run_doctor(
    which: Which = shutil.which,
    runner: Runner = default_runner,
    timeout: float = COMMAND_TIMEOUT_SECONDS,
) -> CommandResult:
    checks = probe_environment(which=which, runner=runner, timeout=timeout)
    return CommandResult("doctor", checks, select_exit_code(checks))

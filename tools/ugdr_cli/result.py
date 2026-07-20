"""Stable command results and subprocess execution for the UGDR harness."""

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, Sequence, Tuple


STATUS_PASS = "PASS"
STATUS_FAIL = "FAIL"
STATUS_SKIP = "SKIP"

ProcessRunner = Callable[
    [Sequence[str], Path, float], subprocess.CompletedProcess
]


@dataclass(frozen=True)
class CheckResult:
    check_id: str
    required: bool
    status: str
    path: str
    diagnostic: str
    remediation: str
    details: Dict[str, object] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, object]:
        value = {
            "id": self.check_id,
            "required": self.required,
            "status": self.status,
            "path": self.path,
            "diagnostic": self.diagnostic,
            "remediation": self.remediation,
        }
        value.update(self.details)
        return value


@dataclass(frozen=True)
class CommandResult:
    command: str
    checks: Tuple[CheckResult, ...]
    exit_code: int

    def to_payload(self) -> Dict[str, object]:
        passed = sum(check.status == STATUS_PASS for check in self.checks)
        failed = sum(check.status == STATUS_FAIL for check in self.checks)
        skipped = sum(check.status == STATUS_SKIP for check in self.checks)
        summary = {
            "total": len(self.checks),
            "passed": passed,
            "failed": failed,
        }
        if skipped:
            summary["skipped"] = skipped
        return {
            "command": self.command,
            "ok": self.exit_code == 0,
            "checks": [check.to_dict() for check in self.checks],
            "summary": summary,
            "exit_code": self.exit_code,
        }


def run_argv(
    command: Sequence[str], cwd: Path, timeout: float
) -> subprocess.CompletedProcess:
    """Run an argv array from a fixed directory without a shell."""
    return subprocess.run(
        list(command),
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
        shell=False,
    )


def render_json(result: CommandResult) -> str:
    return json.dumps(result.to_payload(), ensure_ascii=False, indent=2)


def render_human(result: CommandResult) -> str:
    lines = []
    for check in result.checks:
        lines.append(
            "{} {}: {}".format(check.status, check.check_id, check.diagnostic)
        )
        if check.remediation:
            lines.append("  remediation: {}".format(check.remediation))
    summary = result.to_payload()["summary"]
    summary_text = "summary: {passed} passed, {failed} failed".format(**summary)
    if summary.get("skipped"):
        summary_text += ", {} skipped".format(summary["skipped"])
    lines.append("{}, exit_code={}".format(summary_text, result.exit_code))
    return "\n".join(lines)

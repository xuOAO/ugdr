import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPOSITORY_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from ugdr_cli.bootstrap import run_bootstrap
from ugdr_cli.environment import (  # noqa: E402
    CUDA_REQUIRED_RANGE,
    REQUIRED_BASE_COMMANDS,
    STATUS_FAIL,
    STATUS_PASS,
    render_human,
    render_json,
    run_doctor,
)


class FakeEnvironment:
    def __init__(self, cuda_output="Cuda compilation tools, release 12.3, V12.3.0"):
        commands = list(REQUIRED_BASE_COMMANDS) + ["nvcc", "nvidia-smi"]
        self.paths = {name: "/fake/{}".format(name) for name in commands}
        self.results = {
            "nvcc": subprocess.CompletedProcess(
                ["/fake/nvcc", "--version"], 0, cuda_output, ""
            ),
            "nvidia-smi": subprocess.CompletedProcess(
                ["/fake/nvidia-smi"], 0, "0\n", ""
            ),
        }
        self.timeouts = set()
        self.calls = []

    def which(self, name):
        return self.paths.get(name)

    def run(self, command, timeout):
        name = Path(command[0]).name
        self.calls.append((tuple(command), timeout))
        if name in self.timeouts:
            raise subprocess.TimeoutExpired(command, timeout)
        return self.results[name]


class EnvironmentContractTests(unittest.TestCase):
    def test_cuda_12_3_and_gpu_pass(self):
        fake = FakeEnvironment()
        result = run_doctor(which=fake.which, runner=fake.run)

        self.assertEqual(0, result.exit_code)
        self.assertTrue(result.to_payload()["ok"])
        self.assertEqual(
            ["nvcc", "nvidia-smi"],
            [Path(call[0][0]).name for call in fake.calls],
        )
        cuda = next(check for check in result.checks if check.check_id == "cuda.toolkit")
        self.assertEqual(STATUS_PASS, cuda.status)
        self.assertEqual("12.3", cuda.details["observed_version"])
        self.assertEqual(CUDA_REQUIRED_RANGE, cuda.details["required_range"])

    def test_missing_base_command_has_precedence_and_all_failures_are_reported(self):
        fake = FakeEnvironment("unparseable")
        fake.paths.pop("clang")
        fake.results["nvidia-smi"] = subprocess.CompletedProcess(
            ["/fake/nvidia-smi"], 1, "", "driver unavailable"
        )

        result = run_doctor(which=fake.which, runner=fake.run)
        failures = {
            check.check_id
            for check in result.checks
            if check.status == STATUS_FAIL
        }

        self.assertEqual(10, result.exit_code)
        self.assertIn("base.clang", failures)
        self.assertIn("cuda.toolkit", failures)
        self.assertIn("gpu.nvidia", failures)

    def test_cuda_open_interval_rejects_boundaries(self):
        for observed in ("12.0", "13.0"):
            with self.subTest(observed=observed):
                fake = FakeEnvironment(
                    "Cuda compilation tools, release {}, V{}".format(observed, observed)
                )
                result = run_doctor(which=fake.which, runner=fake.run)
                cuda = next(
                    check for check in result.checks if check.check_id == "cuda.toolkit"
                )
                self.assertEqual(12, result.exit_code)
                self.assertEqual(STATUS_FAIL, cuda.status)
                self.assertEqual(observed, cuda.details["observed_version"])

        build_version = FakeEnvironment("nvcc: V12.0.140")
        result = run_doctor(which=build_version.which, runner=build_version.run)
        self.assertEqual(12, result.exit_code)

    def test_cuda_unparseable_and_timeout_return_12(self):
        unparseable = FakeEnvironment("nvcc output without a release")
        result = run_doctor(which=unparseable.which, runner=unparseable.run)
        self.assertEqual(12, result.exit_code)

        timed_out = FakeEnvironment()
        timed_out.timeouts.add("nvcc")
        result = run_doctor(which=timed_out.which, runner=timed_out.run)
        self.assertEqual(12, result.exit_code)
        self.assertIn("timed out", render_human(result))

    def test_missing_cuda_and_gpu_commands_use_specific_codes(self):
        missing_cuda = FakeEnvironment()
        missing_cuda.paths.pop("nvcc")
        result = run_doctor(which=missing_cuda.which, runner=missing_cuda.run)
        self.assertEqual(12, result.exit_code)

        missing_gpu = FakeEnvironment()
        missing_gpu.paths.pop("nvidia-smi")
        result = run_doctor(which=missing_gpu.which, runner=missing_gpu.run)
        self.assertEqual(13, result.exit_code)

    def test_gpu_unavailable_returns_13(self):
        fake = FakeEnvironment()
        fake.results["nvidia-smi"] = subprocess.CompletedProcess(
            ["/fake/nvidia-smi"], 0, "No devices were found\n", ""
        )
        result = run_doctor(which=fake.which, runner=fake.run)
        self.assertEqual(13, result.exit_code)

    def test_json_is_stable_and_only_cuda_has_version_fields(self):
        first = FakeEnvironment()
        second = FakeEnvironment()
        first_json = render_json(run_doctor(which=first.which, runner=first.run))
        second_json = render_json(run_doctor(which=second.which, runner=second.run))
        self.assertEqual(first_json, second_json)

        payload = json.loads(first_json)
        self.assertEqual(
            ["command", "ok", "checks", "summary", "exit_code"],
            list(payload.keys()),
        )
        for check in payload["checks"]:
            self.assertIn(check["status"], ("PASS", "FAIL"))
            if check["id"] == "cuda.toolkit":
                self.assertIn("observed_version", check)
                self.assertIn("required_range", check)
            else:
                self.assertNotIn("observed_version", check)
                self.assertNotIn("required_range", check)

    def test_bootstrap_is_idempotent(self):
        fake = FakeEnvironment()
        with tempfile.TemporaryDirectory(
            prefix=".ugdr-bootstrap-test-", dir=str(REPOSITORY_ROOT)
        ) as temporary:
            relative_build_dir = Path(temporary).relative_to(REPOSITORY_ROOT)
            first = run_bootstrap(
                REPOSITORY_ROOT,
                relative_build_dir,
                which=fake.which,
                runner=fake.run,
            )
            query = (
                Path(temporary)
                / ".cmake"
                / "api"
                / "v1"
                / "query"
                / "codemodel-v2"
            )
            self.assertEqual(0, first.exit_code)
            self.assertTrue(query.is_file())
            first_mtime = query.stat().st_mtime_ns

            second = run_bootstrap(
                REPOSITORY_ROOT,
                relative_build_dir,
                which=fake.which,
                runner=fake.run,
            )
            self.assertEqual(0, second.exit_code)
            self.assertEqual(first_mtime, query.stat().st_mtime_ns)

    def test_bootstrap_rejects_repository_external_path_without_writing(self):
        fake = FakeEnvironment()
        fake.paths.pop("clang")
        with tempfile.TemporaryDirectory(prefix="ugdr-outside-") as temporary:
            build_dir = Path(temporary) / "build"
            result = run_bootstrap(
                REPOSITORY_ROOT,
                build_dir,
                which=fake.which,
                runner=fake.run,
            )
            self.assertEqual(20, result.exit_code)
            self.assertFalse((build_dir / ".cmake").exists())
            bootstrap = next(
                check for check in result.checks if check.check_id == "bootstrap.file_api"
            )
            self.assertIn("outside the repository", bootstrap.diagnostic)

    def test_bootstrap_reports_repository_local_write_conflict(self):
        fake = FakeEnvironment()
        with tempfile.NamedTemporaryFile(
            prefix=".ugdr-bootstrap-file-", dir=str(REPOSITORY_ROOT)
        ) as conflict:
            relative_conflict = Path(conflict.name).relative_to(REPOSITORY_ROOT)
            result = run_bootstrap(
                REPOSITORY_ROOT,
                relative_conflict,
                which=fake.which,
                runner=fake.run,
            )
            self.assertEqual(20, result.exit_code)
            bootstrap = next(
                check for check in result.checks if check.check_id == "bootstrap.file_api"
            )
            self.assertIn("could not create", bootstrap.diagnostic)

    def test_unknown_command_and_invalid_option_return_2(self):
        for arguments in (["unknown"], ["doctor", "--unknown"]):
            with self.subTest(arguments=arguments):
                completed = subprocess.run(
                    [sys.executable, str(REPOSITORY_ROOT / "tools" / "ugdr")]
                    + arguments,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    check=False,
                )
                self.assertEqual(2, completed.returncode)


if __name__ == "__main__":
    unittest.main()

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPOSITORY_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from ugdr_cli.build import (  # noqa: E402
    EXPECTED_SMOKE_TESTS,
    run_build,
    run_smoke,
    run_test,
)
from ugdr_cli.quality import run_format, run_lint  # noqa: E402
from ugdr_cli.result import STATUS_FAIL, STATUS_PASS, render_json  # noqa: E402


def fake_which(name):
    return name


class RecordingRunner:
    def __init__(self, fail_when=None, smoke_tests=EXPECTED_SMOKE_TESTS):
        self.fail_when = fail_when or (lambda argv: False)
        self.smoke_tests = smoke_tests
        self.calls = []

    def __call__(self, argv, cwd, timeout):
        argv = tuple(argv)
        self.calls.append((argv, Path(cwd), timeout))
        if self.fail_when(argv):
            return subprocess.CompletedProcess(argv, 1, "", "constructed failure")
        if "--show-only=json-v1" in argv:
            payload = {"tests": [{"name": name} for name in self.smoke_tests]}
            return subprocess.CompletedProcess(argv, 0, json.dumps(payload), "")
        return subprocess.CompletedProcess(argv, 0, "", "")


class RepositoryBuildDirectory:
    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".ugdr-quality-test-", dir=str(REPOSITORY_ROOT)
        )
        self.path = Path(self.temporary.name)
        self.relative = self.path.relative_to(REPOSITORY_ROOT).as_posix()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.temporary.cleanup()

    def write_compile_database(self):
        payload = [
            {
                "directory": str(REPOSITORY_ROOT),
                "command": "c++ -c apps/client/main.cpp",
                "file": str(REPOSITORY_ROOT / "apps/client/main.cpp"),
            }
        ]
        (self.path / "compile_commands.json").write_text(
            json.dumps(payload), encoding="utf-8"
        )


class QualityCommandContractTests(unittest.TestCase):
    def test_format_mutates_only_in_default_mode_in_an_isolated_tree(self):
        with tempfile.TemporaryDirectory(prefix="ugdr-format-contract-") as temporary:
            root = Path(temporary)
            source = root / "src/example.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("bad\n", encoding="utf-8")
            formatter = root / "fake-clang-format"
            formatter.write_text(
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "from pathlib import Path\n"
                "files = [Path(value) for value in sys.argv[1:] if not value.startswith('-')]\n"
                "if '--dry-run' in sys.argv:\n"
                "    raise SystemExit(1 if any('bad' in path.read_text() for path in files) else 0)\n"
                "for path in files:\n"
                "    path.write_text(path.read_text().replace('bad', 'good'))\n",
                encoding="utf-8",
            )
            os.chmod(str(formatter), 0o755)
            which = lambda name: str(formatter) if name == "clang-format" else None

            check = run_format(root, check_only=True, which=which)
            self.assertEqual(30, check.exit_code)
            self.assertEqual("bad\n", source.read_text(encoding="utf-8"))

            mutate = run_format(root, check_only=False, which=which)
            self.assertEqual(0, mutate.exit_code)
            self.assertEqual("good\n", source.read_text(encoding="utf-8"))

            clean_check = run_format(root, check_only=True, which=which)
            self.assertEqual(0, clean_check.exit_code)

    def test_format_check_is_stable_and_default_mode_is_the_only_mutating_mode(self):
        first_runner = RecordingRunner()
        second_runner = RecordingRunner()
        first = run_format(
            REPOSITORY_ROOT,
            check_only=True,
            which=fake_which,
            runner=first_runner,
        )
        second = run_format(
            REPOSITORY_ROOT,
            check_only=True,
            which=fake_which,
            runner=second_runner,
        )

        self.assertEqual(0, first.exit_code)
        self.assertEqual(render_json(first), render_json(second))
        self.assertIn("--dry-run", first_runner.calls[0][0])
        self.assertIn("--Werror", first_runner.calls[0][0])
        self.assertNotIn("-i", first_runner.calls[0][0])

        mutating_runner = RecordingRunner()
        result = run_format(
            REPOSITORY_ROOT,
            check_only=False,
            which=fake_which,
            runner=mutating_runner,
        )
        self.assertEqual(0, result.exit_code)
        self.assertIn("-i", mutating_runner.calls[0][0])
        self.assertNotIn("--dry-run", mutating_runner.calls[0][0])

    def test_format_failure_returns_30_with_original_diagnostic(self):
        runner = RecordingRunner(lambda argv: argv[0] == "clang-format")
        result = run_format(
            REPOSITORY_ROOT,
            check_only=True,
            which=fake_which,
            runner=runner,
        )
        self.assertEqual(30, result.exit_code)
        self.assertEqual(STATUS_FAIL, result.checks[0].status)
        self.assertIn("constructed failure", result.checks[0].diagnostic)

    def test_lint_runs_all_read_only_checks(self):
        with RepositoryBuildDirectory() as build:
            build.write_compile_database()
            runner = RecordingRunner()
            result = run_lint(
                REPOSITORY_ROOT,
                build.relative,
                which=fake_which,
                runner=runner,
            )

        self.assertEqual(0, result.exit_code)
        self.assertEqual(
            [
                "lint.format",
                "lint.clang_tidy",
                "lint.module_boundaries",
                "lint.client_contracts",
                "lint.project_docs",
                "lint.skeleton",
                "lint.project_state",
            ],
            [check.check_id for check in result.checks],
        )
        self.assertTrue(all(check.status == STATUS_PASS for check in result.checks))
        flattened = [part for call in runner.calls for part in call[0]]
        self.assertNotIn("-i", flattened)

    def test_each_lint_subcheck_failure_returns_31(self):
        matchers = {
            "lint.format": lambda argv: argv[0] == "clang-format",
            "lint.clang_tidy": lambda argv: argv[0] == "clang-tidy",
            "lint.module_boundaries": lambda argv: any(
                part.endswith("check_module_boundaries.py") for part in argv
            )
            and "--build-dir" in argv,
            "lint.client_contracts": lambda argv: any(
                part.endswith("check_client_contracts.py") for part in argv
            ),
            "lint.project_docs": lambda argv: any(
                part.endswith("check_project_docs.py") for part in argv
            ),
            "lint.skeleton": lambda argv: "--skeleton-only" in argv,
            "lint.project_state": lambda argv: any(
                part.endswith("project_state.py") for part in argv
            ),
        }
        for check_id, matcher in matchers.items():
            with self.subTest(check_id=check_id), RepositoryBuildDirectory() as build:
                build.write_compile_database()
                result = run_lint(
                    REPOSITORY_ROOT,
                    build.relative,
                    which=fake_which,
                    runner=RecordingRunner(matcher),
                )
                self.assertEqual(31, result.exit_code)
                check = next(item for item in result.checks if item.check_id == check_id)
                self.assertEqual(STATUS_FAIL, check.status)
                self.assertIn("constructed failure", check.diagnostic)

    def test_build_test_and_smoke_map_failures_to_contract_codes(self):
        with RepositoryBuildDirectory() as build:
            build_failure = run_build(
                REPOSITORY_ROOT,
                build.relative,
                which=fake_which,
                runner=RecordingRunner(
                    lambda argv: "--build" in argv
                ),
            )
            test_failure = run_test(
                REPOSITORY_ROOT,
                build.relative,
                which=fake_which,
                runner=RecordingRunner(
                    lambda argv: argv[0] == "ctest" and "--show-only=json-v1" not in argv
                ),
            )
            smoke_failure = run_smoke(
                REPOSITORY_ROOT,
                build.relative,
                which=fake_which,
                runner=RecordingRunner(
                    lambda argv: argv[0] == "ctest" and "--show-only=json-v1" not in argv
                ),
            )

        self.assertEqual(32, build_failure.exit_code)
        self.assertEqual(33, test_failure.exit_code)
        self.assertEqual(34, smoke_failure.exit_code)

    def test_configure_failure_maps_to_each_command_code_and_skips_dependents(self):
        for command, expected in ((run_build, 32), (run_test, 33), (run_smoke, 34)):
            with self.subTest(command=command.__name__), RepositoryBuildDirectory() as build:
                result = command(
                    REPOSITORY_ROOT,
                    build.relative,
                    which=fake_which,
                    runner=RecordingRunner(
                        lambda argv: argv[0] == "cmake" and "-S" in argv
                    ),
                )
                self.assertEqual(expected, result.exit_code)
                self.assertEqual(STATUS_FAIL, result.checks[0].status)
                self.assertTrue(
                    all(check.status == "SKIP" for check in result.checks[1:])
                )

    def test_smoke_selects_only_handoff_client_and_daemon(self):
        with RepositoryBuildDirectory() as build:
            runner = RecordingRunner()
            result = run_smoke(
                REPOSITORY_ROOT,
                build.relative,
                which=fake_which,
                runner=runner,
            )

        self.assertEqual(0, result.exit_code)
        selection = next(
            check for check in result.checks if check.check_id == "smoke.selection"
        )
        self.assertEqual(list(sorted(EXPECTED_SMOKE_TESTS)), selection.details["selected_tests"])
        ctest_calls = [call[0] for call in runner.calls if call[0][0] == "ctest"]
        self.assertEqual(2, len(ctest_calls))
        self.assertTrue(all("^ugdr_smoke$" in call for call in ctest_calls))
        flattened = [part for call in runner.calls for part in call[0]]
        forbidden_fragments = (
            "clang-format",
            "clang-tidy",
            "check_module_boundaries.py",
            "check_project_docs.py",
            "project_state.py",
        )
        for fragment in forbidden_fragments:
            self.assertFalse(any(part.endswith(fragment) for part in flattened))

    def test_smoke_rejects_label_scope_drift(self):
        with RepositoryBuildDirectory() as build:
            result = run_smoke(
                REPOSITORY_ROOT,
                build.relative,
                which=fake_which,
                runner=RecordingRunner(smoke_tests=("ugdr_client_smoke",)),
            )
        self.assertEqual(34, result.exit_code)
        selection = next(
            check for check in result.checks if check.check_id == "smoke.selection"
        )
        self.assertEqual(STATUS_FAIL, selection.status)

    def test_build_related_commands_reject_unsafe_paths_before_running(self):
        unsafe_paths = (
            REPOSITORY_ROOT,
            REPOSITORY_ROOT / "src",
            REPOSITORY_ROOT.parent / "outside-build",
        )
        commands = (run_build, run_test, run_smoke)
        for command in commands:
            for path in unsafe_paths:
                with self.subTest(command=command.__name__, path=path):
                    runner = RecordingRunner()
                    result = command(
                        REPOSITORY_ROOT,
                        path,
                        which=fake_which,
                        runner=runner,
                    )
                    self.assertNotEqual(0, result.exit_code)
                    self.assertEqual([], runner.calls)

    def test_json_contract_has_stable_top_level_fields(self):
        runner = RecordingRunner()
        result = run_format(
            REPOSITORY_ROOT,
            check_only=True,
            which=fake_which,
            runner=runner,
        )
        payload = json.loads(render_json(result))
        self.assertEqual(
            ["command", "ok", "checks", "summary", "exit_code"],
            list(payload.keys()),
        )

    def test_new_commands_reject_unknown_arguments_with_2(self):
        for command in ("format", "lint", "build", "test", "smoke"):
            with self.subTest(command=command):
                completed = subprocess.run(
                    [
                        sys.executable,
                        str(REPOSITORY_ROOT / "tools/ugdr"),
                        command,
                        "--unknown",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    check=False,
                )
                self.assertEqual(2, completed.returncode)


if __name__ == "__main__":
    unittest.main()

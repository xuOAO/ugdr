import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPOSITORY_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from ugdr_cli.git_workflow import (  # noqa: E402
    GitWorkflowError,
    PullRequestContext,
    action_branch_name,
    ensure_action_branch,
    reconcile_merged_pr,
    validate_commit_scope,
    validate_pr_action,
)
from ugdr_cli.workflow import (  # noqa: E402
    WORKFLOW_EXIT_CODE,
    run_state_core,
    state_next,
    state_show,
)


def state_value(state="awaiting_review", next_actions=None, blockers=None):
    if next_actions is None:
        next_actions = (
            {"F01": [{"step": "F01-S06", "action": "验收"}]}
            if state == "completed"
            else {}
        )
    return {
        "schema_version": 1,
        "version": "v1",
        "feature": "F01",
        "step": "F01-S05",
        "state": state,
        "next_actions": next_actions,
        "blockers": ([] if state != "blocked" else ["external approval"])
        if blockers is None
        else blockers,
        "updated_at": "2026-07-20T12:00:00+08:00",
        "updated_by": "agent",
    }


class WorkflowFixture:
    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ugdr-workflow-")
        self.root = Path(self.temporary.name)
        (self.root / "docs/status").mkdir(parents=True)
        (self.root / "tools").mkdir()
        shutil.copyfile(
            REPOSITORY_ROOT / "docs/status/current.schema.json",
            self.root / "docs/status/current.schema.json",
        )
        shutil.copyfile(
            REPOSITORY_ROOT / "tools/workflow-rules.json",
            self.root / "tools/workflow-rules.json",
        )
        shutil.copyfile(
            REPOSITORY_ROOT / "tools/project_state.py",
            self.root / "tools/project_state.py",
        )
        shutil.copyfile(
            REPOSITORY_ROOT / "tools/project_roadmap.py",
            self.root / "tools/project_roadmap.py",
        )
        source = self.root / "docs/v1_docs/roadmap.md"
        source.parent.mkdir(parents=True)
        source.write_text(
            "---\nreview_status: reviewed\nsource_revision: 1\n"
            "generated_body_sha256: {}\n---\n# Roadmap\n".format("b" * 64),
            encoding="utf-8",
        )
        self.write_roadmap({"F01": [{"step": "F01-S06", "action": "验收"}]})
        self.write_state(state_value())
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.temporary.cleanup()

    def write_state(self, value):
        (self.root / "docs/status/current.json").write_text(
            json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )

    def read_state(self):
        return json.loads((self.root / "docs/status/current.json").read_text(encoding="utf-8"))

    def write_roadmap(self, next_actions):
        source = "docs/v1_docs/roadmap.md"
        routes = [
            {
                "version": "v1",
                "feature": "F01",
                "step": "F01-S05",
                "source": source,
                "next_actions": next_actions,
            }
        ]
        targets = {
            action if isinstance(action, str) else action["step"]
            for actions in next_actions.values()
            for action in actions
        }
        routes.extend(
            {
                "version": "v1",
                "feature": step.split("-", 1)[0],
                "step": step,
                "source": source,
                "next_actions": {},
            }
            for step in sorted(targets)
        )
        value = {
            "schema_version": 1,
            "reviewed_sources": [
                {"path": source, "revision": 1, "body_sha256": "b" * 64}
            ],
            "routes": routes,
        }
        (self.root / "docs/status/roadmap.json").write_text(
            json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )


class RecordingRunner:
    def __init__(self, responses):
        self.responses = {tuple(key): list(value) for key, value in responses.items()}
        self.calls = []

    def __call__(self, argv, cwd, timeout):
        key = tuple(argv)
        self.calls.append(key)
        values = self.responses.get(key)
        if not values:
            return subprocess.CompletedProcess(argv, 0, "", "")
        return values.pop(0)


def completed(argv, stdout="", stderr="", code=0):
    return subprocess.CompletedProcess(argv, code, stdout, stderr)


class WorkflowDecisionTests(unittest.TestCase):
    def test_show_exposes_stable_scope_state_and_gates(self):
        with WorkflowFixture() as fixture:
            payload, exit_code = state_show(fixture.root)
        self.assertEqual(0, exit_code)
        self.assertEqual("state.show", payload["command"])
        self.assertEqual("F01-S05", payload["scope"]["step"])
        self.assertEqual("awaiting_review", payload["state"])
        self.assertEqual([], payload["blockers"])

    def test_each_stable_state_has_a_deterministic_action(self):
        expected = {
            "awaiting_review": ("review_design", True),
            "ready_for_implementation": ("implement_and_verify", False),
            "awaiting_acceptance": ("accept_implementation", True),
            "blocked": ("resolve_blockers", True),
        }
        with WorkflowFixture() as fixture:
            for state, action in expected.items():
                fixture.write_state(state_value(state))
                payload, exit_code = state_next(fixture.root)
                with self.subTest(state=state):
                    self.assertEqual(0, exit_code)
                    self.assertEqual(action[0], payload["action"])
                    self.assertEqual(action[1], payload["requires_human"])

    def test_completed_zero_single_and_multiple_actions_are_distinct(self):
        cases = (
            ({}, "no_next_action", True),
            ({"F01": [{"step": "F01-S06", "action": "验收"}]}, "advance_scope", False),
            (
                {
                    "F01": [
                        {"step": "F01-S06", "action": "验收"},
                        {"step": "F01-S07", "action": "另一路径"},
                    ]
                },
                "select_next_action",
                True,
            ),
        )
        with WorkflowFixture() as fixture:
            for next_actions, action, requires_human in cases:
                fixture.write_roadmap(next_actions)
                fixture.write_state(state_value("completed", next_actions))
                payload, exit_code = state_next(fixture.root)
                with self.subTest(action=action):
                    self.assertEqual(0, exit_code)
                    self.assertEqual(action, payload["action"])
                    self.assertEqual(requires_human, payload["requires_human"])

    def test_invalid_rules_return_workflow_exit_35(self):
        with WorkflowFixture() as fixture:
            (fixture.root / "tools/workflow-rules.json").write_text(
                '{"schema_version": 1, "states": {}}', encoding="utf-8"
            )
            payload, exit_code = state_next(fixture.root)
        self.assertEqual(WORKFLOW_EXIT_CODE, exit_code)
        self.assertFalse(payload["ok"])
        self.assertEqual("workflow rules are invalid", payload["reason"])

    def test_transition_delegates_to_existing_state_core_and_preserves_gates(self):
        with WorkflowFixture() as fixture:
            reviewed = fixture.root / "docs/reviewed.md"
            reviewed.write_text(
                '---\nreview_status: "reviewed"\n---\n# Reviewed\n', encoding="utf-8"
            )
            payload, exit_code = run_state_core(
                fixture.root,
                "transition",
                [
                    "--to",
                    "ready_for_implementation",
                    "--updated-by",
                    "agent",
                    "--reviewed-doc",
                    "docs/reviewed.md",
                ],
            )
            self.assertEqual(0, exit_code)
            self.assertTrue(payload["ok"])
            self.assertEqual("ready_for_implementation", fixture.read_state()["state"])

            before = fixture.read_state()
            payload, exit_code = run_state_core(
                fixture.root,
                "transition",
                ["--to", "awaiting_acceptance", "--updated-by", "agent"],
            )
            self.assertEqual(WORKFLOW_EXIT_CODE, exit_code)
            self.assertFalse(payload["ok"])
            self.assertEqual(before, fixture.read_state())

    def test_advance_scope_requires_human_confirmation(self):
        with WorkflowFixture() as fixture:
            fixture.write_state(
                state_value(
                    "completed",
                    {"F01": [{"step": "F01-S06", "action": "验收"}]},
                )
            )
            payload, exit_code = run_state_core(
                fixture.root,
                "advance-scope",
                [
                    "--to",
                    "awaiting_review",
                    "--updated-by",
                    "human",
                    "--version",
                    "v1",
                    "--feature",
                    "F01",
                    "--step",
                    "F01-S06",
                    "--human-confirmed",
                ],
            )
            self.assertEqual(0, exit_code)
            self.assertTrue(payload["ok"])
            self.assertEqual("F01-S06", fixture.read_state()["step"])


class GitWorkflowTests(unittest.TestCase):
    def test_branch_name_is_stable_and_scoped(self):
        self.assertEqual(
            "codex/f01-s06-clean-workspace",
            action_branch_name("F01-S06", "Clean workspace"),
        )

    def test_action_branch_requires_clean_synchronized_master(self):
        runner = RecordingRunner(
            {
                ("git", "status", "--porcelain=v1"): [completed([], "")],
                ("git", "rev-parse", "master"): [completed([], "abc\n")],
                ("git", "rev-parse", "origin/master"): [completed([], "abc\n")],
                ("git", "worktree", "list", "--porcelain"): [
                    completed([], "worktree /tmp/current\nbranch refs/heads/master\n")
                ],
                ("git", "branch", "--list", "codex/f01-s06-clean-workspace"): [
                    completed([], "")
                ],
            }
        )
        branch = ensure_action_branch(
            Path("/tmp/current"), "F01-S06", "Clean workspace", runner=runner
        )
        self.assertEqual("codex/f01-s06-clean-workspace", branch)
        self.assertIn(
            ("git", "switch", "-c", branch, "master"),
            runner.calls,
        )

    def test_branch_in_another_worktree_is_rejected(self):
        target = "codex/f01-s06-clean-workspace"
        runner = RecordingRunner(
            {
                ("git", "status", "--porcelain=v1"): [completed([], "")],
                ("git", "rev-parse", "master"): [completed([], "abc\n")],
                ("git", "rev-parse", "origin/master"): [completed([], "abc\n")],
                ("git", "worktree", "list", "--porcelain"): [
                    completed([], "worktree /tmp/other\nbranch refs/heads/{}\n".format(target))
                ],
            }
        )
        with self.assertRaisesRegex(GitWorkflowError, "already checked out"):
            ensure_action_branch(
                Path("/tmp/current"), "F01-S06", "Clean workspace", runner=runner
            )

    def test_commit_scope_rejects_mixed_and_out_of_scope_changes(self):
        self.assertEqual(
            ("tools/workflow-rules.json",),
            validate_commit_scope(
                ["tools/workflow-rules.json"],
                ["tools/workflow-rules.json"],
                ["tools"],
            ),
        )
        with self.assertRaisesRegex(GitWorkflowError, "exactly match"):
            validate_commit_scope(["tools/a", "notes.txt"], ["tools/a"], ["tools"])
        with self.assertRaisesRegex(GitWorkflowError, "outside"):
            validate_commit_scope(["notes.txt"], ["notes.txt"], ["tools"])

    def test_pr_lifecycle_uses_one_draft_and_requires_final_acceptance(self):
        self.assertEqual(
            "create_draft", validate_pr_action("create_draft", None, review_gate=True)
        )
        draft = PullRequestContext(2, "OPEN", True, "master")
        self.assertEqual("update_draft", validate_pr_action("update_draft", draft))
        with self.assertRaisesRegex(GitWorkflowError, "final human acceptance"):
            validate_pr_action("mark_ready", draft)
        self.assertEqual(
            "mark_ready", validate_pr_action("mark_ready", draft, acceptance_gate=True)
        )
        with self.assertRaisesRegex(GitWorkflowError, "already has a PR"):
            validate_pr_action("create_draft", draft, review_gate=True)
        closed = PullRequestContext(2, "CLOSED", True, "master")
        with self.assertRaisesRegex(GitWorkflowError, "must be open"):
            validate_pr_action("update_draft", closed)
        with self.assertRaisesRegex(GitWorkflowError, "never merges"):
            validate_pr_action("merge", draft)

    def test_reconciliation_rejects_unmerged_dirty_unpushed_and_remote_ahead(self):
        root = Path("/tmp/reconcile")
        open_pr = PullRequestContext(2, "OPEN", True, "master")
        with self.assertRaisesRegex(GitWorkflowError, "only a PR merged"):
            reconcile_merged_pr(root, open_pr, runner=RecordingRunner({}))

        merged = PullRequestContext(2, "MERGED", False, "master")
        dirty = RecordingRunner(
            {("git", "status", "--porcelain=v1"): [completed([], " M file\n")]}
        )
        with self.assertRaisesRegex(GitWorkflowError, "clean"):
            reconcile_merged_pr(root, merged, runner=dirty)

        for counts, message in (
            ("0 1\n", "unpushed"),
            ("1 0\n", "behind"),
            ("1 1\n", "unpushed"),
        ):
            runner = RecordingRunner(
                {
                    ("git", "status", "--porcelain=v1"): [completed([], "")],
                    ("git", "branch", "--show-current"): [completed([], "feature\n")],
                    (
                        "git",
                        "rev-list",
                        "--left-right",
                        "--count",
                        "origin/feature...HEAD",
                    ): [completed([], counts)],
                }
            )
            with self.subTest(counts=counts), self.assertRaisesRegex(
                GitWorkflowError, message
            ):
                reconcile_merged_pr(root, merged, runner=runner)

    def test_merged_pr_reconciles_with_fetch_switch_and_fast_forward(self):
        with WorkflowFixture() as fixture:
            fixture.write_state(state_value("completed"))
            merged = PullRequestContext(2, "MERGED", False, "master")
            runner = RecordingRunner(
                {
                    ("git", "status", "--porcelain=v1"): [completed([], "")],
                    ("git", "branch", "--show-current"): [completed([], "feature\n")],
                    (
                        "git",
                        "rev-list",
                        "--left-right",
                        "--count",
                        "origin/feature...HEAD",
                    ): [completed([], "0 0\n")],
                }
            )
            reconcile_merged_pr(fixture.root, merged, runner=runner)
            for call in (
                ("git", "fetch", "origin", "feature"),
                ("git", "fetch", "origin", "master"),
                ("git", "switch", "master"),
                ("git", "merge", "--ff-only", "origin/master"),
            ):
                self.assertIn(call, runner.calls)


class SkillContractTests(unittest.TestCase):
    def test_project_skill_contains_human_and_pr_gates(self):
        skill = (
            REPOSITORY_ROOT / ".agents/skills/ugdr-continue-project/SKILL.md"
        ).read_text(encoding="utf-8")
        for required in (
            "继续项目",
            "tools/ugdr state next --json",
            "已完成审阅",
            "已实现",
            "Draft PR",
            "Ready for review",
            "绝不直接 push `master`、自动 merge",
        ):
            self.assertIn(required, skill)


if __name__ == "__main__":
    unittest.main()

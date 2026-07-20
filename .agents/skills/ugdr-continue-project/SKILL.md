---
name: ugdr-continue-project
description: 从仓库机器状态和 Git/PR 上下文继续推进 UGDR 项目，连续执行当前 scope 内允许的设计、同步、实现、验证、提交和交接动作，并在选择、飞书审阅、权限、失败或最终人工验收门禁停止。用于用户说“继续项目”“继续”“推进”“审阅通过”“验收通过”“已合并”，或要求恢复 UGDR 新会话、处理 next_actions、对账已合并 PR 时。
---

# 继续 UGDR 项目

从仓库根目录运行。将 `docs/status/current.json` 视为项目状态事实，将远端 PR 视为 Git 合并事实，将飞书待办视为人工审阅和实现确认事实；不得从聊天历史、测试或 commit 推断人工门禁。

## 入口预检

1. 读取仓库 `AGENTS.md`、`docs/status/current.json` 和当前步骤的已审阅 Markdown。
2. 运行 `tools/ugdr state show --json` 与 `tools/ugdr state next --json`。命令失败时停止，不直接编辑 `current.json`。
3. 运行只读 Git 检查：当前分支、工作区、上游 ahead/behind、worktree 占用和当前分支 PR。工作区混杂、未推送提交或分叉时停止；不得 reset、rebase、强推或删除分支。
4. 先处理已合并 PR 的对账，再按项目状态选择动作。

## 按状态推进

### `completed`

- 当前功能分支存在面向 `master` 的未合并 Ready PR：等待用户手动 merge，不切换 `master`，不开始下一 action。
- PR 已合并：确认工作区干净且无未推送提交；fetch，切换 `master`，仅用 fast-forward 更新；再次运行 `tools/ugdr state show --json` 和项目状态校验。任何不一致都停止。
- 已位于同步后的 `master`：读取 `next_actions`。没有 action 时报告；多个 action 时要求用户选择；唯一 action 且用户说“继续”时视为明确选择。
- 开始新 scope 前检查目标分支是否已存在或被其他 worktree 占用。基于最新 `master` 创建或恢复 `codex/<step>-<slug>`，再用 `tools/ugdr state advance-scope` 进入 `awaiting_review`。不得在旧功能分支直接开始新 action。

### `awaiting_review`

- 未收到本轮明确审阅通过：完善飞书设计并停在审阅门禁。
- 收到明确审阅通过：读取 `.agents/skills/lark-account-registry/SKILL.md` 解析唯一账户；使用 `ugdr-sync-docs-to-md` 重新核验飞书中唯一的“已完成审阅”已勾选并同步 Markdown。
- 同步和哈希校验通过后，用 `tools/ugdr state transition --to ready_for_implementation` 提交状态交接；枚举 diff，只提交当前 scope，push 当前功能分支。
- 查询当前分支 PR；不得创建第二个 PR。不存在时创建以 `master` 为 base 的 Draft PR。创建失败时停止，不得宣称交接完成或继续实现。

### `ready_for_implementation`

- 要求当前分支已经有一个 open Draft PR；否则停止。
- 严格按已审阅 Markdown 实现当前 scope。普通可定位错误可以有界修复；设计变化、权限、不可修复失败或混杂改动时停止。
- 执行步骤文档规定的验证和仓库治理命令。只有全部必需验证通过后，记录 `docs/progress/Fxx-Sxx.md` 的 Source、Delivered、Verification、Deviations/Remaining；不要填写 Acceptance。
- 用 `tools/ugdr state transition --to awaiting_acceptance --updated-by agent --verification-passed` 更新状态；枚举并验证提交路径，自动 commit、push，以更新同一个 Draft PR。保持 Draft，不标记 Ready，不自动 merge。

### `awaiting_acceptance`

- 未收到本轮明确验收通过：展示 diff、验证与 progress 摘要后停止。
- 收到明确验收通过：重新读取飞书步骤文档，要求唯一的“已实现”已勾选；口头确认不能替代待办。
- 门禁满足后补充 progress Acceptance，并用 `tools/ugdr state transition --to completed --updated-by human --human-confirmed` 更新状态。
- 创建最终 commit、push 更新同一个 Draft PR，再将它标记为 Ready for review。任一步失败时停止；不得创建第二个 PR、自动 merge、切换 `master` 或开始下一 action。

### `blocked`

报告全部 blockers、原恢复目标和所需外部动作。不得自行清空 blocker；恢复时仍需满足目标状态对应的飞书、验证或人工门禁。

## Git 与 PR 安全约束

- 自动 commit 前运行 `git status --short`、查看 diff，并只暂存当前 scope 的明确路径。同文件存在无法可靠拆分的其他工作时停止。
- 审阅交接、实现提交和最终验收只使用同一功能分支与同一 PR。
- 审阅通过后创建 Draft PR；实现阶段只更新 Draft PR；最终人工验收且“已实现”勾选后才标记 Ready。
- 绝不直接 push `master`、自动 merge、根据 PR review 推断最终验收，或为 merge 新增项目稳定状态。

## 停止交接

停止时返回：当前 scope 与状态、所在分支和 PR 状态、已完成动作、验证证据、未完成动作、停止原因，以及用户下一步需要做的唯一明确事项。不要生成临时状态文件或聊天流水；只有跨会话有价值的实施事实进入 `docs/progress/`。

# Repository Skeleton

This document is the human-readable entry point for the repository skeleton. It records stable module responsibilities and dependency intent; it does not list ordinary source files, runtime interfaces, implementation classes, or project progress.

`tools/module-boundaries.json` is the machine-readable source for required paths, production targets, and allowed production dependencies. The generated section below must match that policy. Change it only when a module boundary, target name, required top-level path, or allowed dependency changes.

`tools/ugdr` is the repository-local Python entry for development harness commands. F01-S03 owns its `bootstrap` and `doctor` subcommands; F01-S04 adds `format`, `lint`, `build`, `test`, and `smoke`; F01-S05 adds deterministic `state show`, `state next`, `state transition`, and `state advance-scope` commands backed by `tools/workflow-rules.json` and the existing state core. `docs/status/roadmap.json` binds machine routes to reviewed Markdown revisions and hashes; completion derives `current.json.next_actions` from that roadmap, while `state reconcile-roadmap` repairs legacy drift without guessing from chat or prose. Their implementation remains under `tools/ugdr_cli`, uses `.clang-format` and `.clang-tidy` as repository quality rules, and does not change the production target dependency graph. `.agents/skills/ugdr-continue-project/` supplies the Agent-facing orchestration loop, while Git/PR safety helpers remain testable repository tooling rather than project-state facts.

`ugdr_ipc` is the business-neutral local IPC layer. It owns the Unix Domain `SOCK_SEQPACKET`
envelope, `SCM_RIGHTS` fd transfer, and synchronous client/single-threaded server types.
`ugdr_control` depends on it only through the typed UGDR adapter; verbs resources remain outside
F03-S01. F03-S02 adds generation-safe typed registries and Device/Context control semantics;
F03-S03 adds PD/MR/CQ lifecycle, daemon key lookup, and UUID-routed CUDA IPC mappings. `ugdr_api`
uses the control boundary without depending directly on the generic IPC layer and uses `ugdr_gpu`
only for Client-side CUDA allocation export.

F04-S01 adds `ugdr_queue` as the shared data-plane layout boundary. It owns versioned SPSC ring
memory, descriptor slots, and mapping validation; `ugdr_control` only creates and transfers those
mappings, while public post and poll operations remain outside this step.

<!-- BEGIN GENERATED: module-boundaries -->
## Repository areas

| Path | Responsibility |
|---|---|
| `include/ugdr` | 对外公共声明 |
| `src` | 生产库实现，按模块目录隔离 |
| `apps` | 可执行程序入口与组合根 |
| `tests/unit` | 模块级单元测试 |
| `tests/integration` | 跨模块与边界集成测试 |
| `tests/smoke` | 仓库主路径最小运行检查 |
| `tools` | 机器可执行的仓库规则与检查器 |
| `docs/architecture` | 稳定架构与仓库边界说明 |
| `docs/decisions` | 长期有效的项目决策记录 |
| `docs/contracts` | 已审阅的 Client 可观察 API 与行为契约 |
| `docs/governance` | 机器可检查的文档治理规则 |
| `docs/progress` | 执行过程与验证交接记录 |
| `docs/status` | 机器可读当前状态与受审阅路线图 |
| `docs/v1_docs` | 已审阅 v1 设计的执行快照与索引 |

## Production targets

| Target | Path | Responsibility |
|---|---|---|
| `ugdr_api` | `src/api` | Client 可见 API、Device/Context proxy 与进程级控制连接 |
| `ugdr_ipc` | `src/ipc` | 通用本机 IPC 协议、fd 传输与 client/server 封装 |
| `ugdr_queue` | `src/queue` | 版本化共享 ring 布局、直接 descriptor slot 与跨进程 mapping 原语 |
| `ugdr_control` | `src/control` | UGDR 控制语义 adapter、类型化对象注册表与 Device/Context 服务 |
| `ugdr_worker` | `src/worker` | 数据面 Worker 占位 |
| `ugdr_gpu` | `src/gpu` | CUDA allocation 导出、per-GPU runtime context 与 IPC mapping backend |
| `ugdr_client` | `apps/client` | 最小 Client 可执行文件 |
| `ugdr_daemon` | `apps/daemon` | Control、Worker 与 GPU 的组合根 |

## Allowed production dependencies

| Caller | Allowed dependencies |
|---|---|
| `ugdr_api` | `ugdr_control`, `ugdr_ipc`, `ugdr_gpu`, `ugdr_queue` |
| `ugdr_ipc` | None |
| `ugdr_queue` | None |
| `ugdr_control` | `ugdr_ipc`, `ugdr_queue` |
| `ugdr_worker` | `ugdr_control`, `ugdr_ipc`, `ugdr_queue` |
| `ugdr_gpu` | None |
| `ugdr_client` | `ugdr_api`, `ugdr_control`, `ugdr_ipc`, `ugdr_gpu`, `ugdr_queue` |
| `ugdr_daemon` | `ugdr_control`, `ugdr_ipc`, `ugdr_worker`, `ugdr_gpu`, `ugdr_queue` |
<!-- END GENERATED: module-boundaries -->

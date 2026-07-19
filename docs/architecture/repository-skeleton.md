# Repository Skeleton

This document is the human-readable entry point for the repository skeleton. It records stable module responsibilities and dependency intent; it does not list ordinary source files, runtime interfaces, implementation classes, or project progress.

`tools/module-boundaries.json` is the machine-readable source for required paths, production targets, and allowed production dependencies. The generated section below must match that policy. Change it only when a module boundary, target name, required top-level path, or allowed dependency changes.

`tools/ugdr` is the repository-local Python entry for development harness commands. F01-S03 owns its `bootstrap` and `doctor` subcommands; their implementation remains under `tools/ugdr_cli` and does not change the production target dependency graph.

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

## Production targets

| Target | Path | Responsibility |
|---|---|---|
| `ugdr_api` | `src/api` | Client 可见 API 门面占位 |
| `ugdr_control` | `src/control` | 控制面元数据与资源管理占位 |
| `ugdr_worker` | `src/worker` | 数据面 Worker 占位 |
| `ugdr_gpu` | `src/gpu` | CUDA 执行模块占位 |
| `ugdr_client` | `apps/client` | 最小 Client 可执行文件 |
| `ugdr_daemon` | `apps/daemon` | Control、Worker 与 GPU 的组合根 |

## Allowed production dependencies

| Caller | Allowed dependencies |
|---|---|
| `ugdr_api` | None |
| `ugdr_control` | None |
| `ugdr_worker` | `ugdr_control` |
| `ugdr_gpu` | None |
| `ugdr_client` | `ugdr_api` |
| `ugdr_daemon` | `ugdr_control`, `ugdr_worker`, `ugdr_gpu` |
<!-- END GENERATED: module-boundaries -->

---
source_kind: "feishu-docx"
source_token: "A3SddhG5CochALxSqHRcD0Nzn8e"
source_url: "https://my.feishu.cn/docx/A3SddhG5CochALxSqHRcD0Nzn8e"
source_path: "我的空间 / UGDR / UGDR_v1 设计 / F01_项目初始化与开发 Harness / F01_项目初始化与开发 Harness 功能文档"
source_title: "F01_项目初始化与开发 Harness 功能文档"
source_revision: 32
doc_type: "feature"
content_mode: "agent"
review_status: "reviewed"
synced_at: "2026-07-18T23:12:13+08:00"
generated_by: "ugdr-sync-docs-to-md"
generated_body_sha256: "facd56388f1fefdd16dae21b5e2e6e70ae3d4703ba68143a4546337b9a31fa70"
---
# F01_项目初始化与开发 Harness 功能文档

**所属版本：** UGDR_v1

**所属版本文档：** [UGDR_v1 版本文档](../UGDR_v1_版本文档.md)

**功能标识：** F01

**功能名称：** 项目初始化与开发 Harness

## 一、功能目标

该功能完成后，UGDR 开发者和新的 Agent 会话能够在干净 workspace 中快速识别项目结构与当前阶段，通过统一入口完成环境诊断、格式检查、静态检查、构建、测试和 smoke check，并从独立状态载体恢复下一项工作。目标以仓库骨架可编译、命令退出状态稳定、失败信息可操作，以及新会话无需依赖聊天历史即可执行基础验证为完成判断。

## 二、背景与版本关系

F01 是 UGDR_v1 的开发前置层，承接版本文档中“先建立可重复执行的项目初始化与开发 Harness”的目标。当前 workspace 尚无业务代码；若直接进入 API、控制面或数据路径实现，模块边界、项目知识、状态交接和验证入口会在不同步骤中漂移。F01 先建立人和 Agent 都能读取、执行、验证和持续交接的工程基础，完成人工验收后，F02-F07 才进入正式实现。

该功能只负责开发环境与仓库级 Harness，不属于两个 client 加一个 daemon 的 UGDR 运行时数据路径。

## 三、功能范围

- 建立与 v1 功能边界一致的可编译仓库骨架：公共接口位于 include/ugdr，业务模块按 api、control、queues、worker、gpu 分区，运行入口按 client、daemon 分区，测试按 unit、integration、smoke 分层；F01 只建立边界和最小占位，不实现后续功能。
- 固定基础工具链为 C++20、CUDA C++、CMake、Ninja、CTest、clang-format 和 clang-tidy，并通过统一入口屏蔽具体命令组合。
- 提供精简 AGENTS.md 项目地图、docs/v1_docs 版本设计入口、docs/status/current.md 当前状态、docs/decisions 决策记录和 docs/progress 执行进度，使长期规则、当前状态、过程记录和临时计划相互分离。
- 提供 Agent 中立的 Python CLI tools/ugdr，统一暴露 bootstrap、doctor、format、lint、build、test 和 smoke 子命令；Agent 专属 Skills 或配置只能作为附加入口。
- 所有统一命令具有稳定退出状态和可操作的失败信息；doctor 能区分缺少工具、版本不满足、CUDA 不可用和 GPU 不可用。
- 建立基础质量门禁，包括格式检查、静态检查、最小构建、CTest、模块依赖方向、目录边界、文档规范和骨架同步检查。完整文档治理放入 lint；smoke 只执行新会话继续工作所需的最小生存检查。
- 建立仓库级可复用工作流和新会话交接路径，使新的 Agent 会话无需依赖聊天历史即可定位当前阶段、下一项工作和基础验证入口。

## 四、非目标

- 不实现 UGDR verbs-like API、对象模型、daemon 管理逻辑、WQ/RQ/CQ、loop worker、GPU memcpy kernel 或运行时数据路径。
- 不在 F01 中确定后续功能的接口字段、线程模型、队列内存序、datagram 布局、文件、类或函数实现。
- 不要求生产级 CI/CD、性能 benchmark、双机环境、daemon crash 或 peer disconnect 故障注入；这些能力由后续功能按需补充。
- 不把完整项目知识、可变状态或执行进度堆入 AGENTS.md，也不让 Codex 或其他单一 Agent 的专属配置成为唯一入口。
- bootstrap 和 doctor 不承诺自动修复所有系统依赖；无法自动处理的环境问题必须给出明确诊断和人工操作方向。

## 五、依赖与约束

- 来源依赖：已审阅的 UGDR_v1 版本文档、UGDR 项目Agent coding 工作明细和 UGDRv1 项目设计。
- 前置关系：F01 是 F02-F07 的正式实现前置；F01 未通过人工验收时，不进入后续功能实现。
- 工具链约束：使用 C++20、CUDA C++、CMake、Ninja、CTest、clang-format、clang-tidy 和 Python 3；具体最低版本和安装方式在对应步骤文档确认。
- 环境约束：目标环境为当前 Linux 开发 workspace，v1 以本机环境为依据，不建立跨 Linux 主机的支持矩阵。doctor 必须区分主机工具链、CUDA Toolkit 和真实 GPU 的可用状态；缺少 GPU/CUDA 时不得伪造通过。
- 知识约束：仓库中的可版本化内容是 Agent 执行知识源；飞书文档经人工确认后同步到 Markdown，再作为编码依据。
- 状态约束：长期规则、当前状态、执行进度和临时计划分别维护；状态载体和命令输出必须便于新会话恢复。
- 兼容性约束：tools/ugdr 是稳定用户入口，底层 CMake、Ninja、CTest 或 lint 参数可以演进，但不得要求使用者记忆多套内部命令。

## 六、功能设计与模块边界

F01 由仓库骨架、统一命令层、知识与状态层、质量 Harness 四个边界组成。它们只提供后续功能可依赖的结构和执行契约，不包含 UGDR 运行时业务实现。

| 边界 | 职责 | 输入 | 输出与失败语义 |
|-|-|-|-|
| 仓库骨架 | 建立 include/ugdr、api、control、queues、worker、gpu、client、daemon 和测试分层的允许边界、最小占位与依赖方向，不固化后续运行时实现细节。 | UGDR_v1 功能划分和骨架说明文档。 | 可配置、可编译的最小目标和边界检查；实际骨架与文档声明不一致，或出现越界依赖时，lint 失败。 |
| 统一命令层 | 由 Python CLI tools/ugdr 编排 bootstrap、doctor、format、lint、build、test 和 smoke。lint 承载完整静态一致性检查；smoke 只执行新会话继续工作所需的最小生存检查。 | 子命令、workspace 状态和环境能力。 | 稳定退出码、清晰日志和可操作诊断；未知命令、缺少依赖、lint 违规或执行失败返回非零。 |
| 知识与状态层 | 用精简 AGENTS.md 导航版本文档、当前状态、决策和进度记录，分离长期规则与可变状态，并为文档长度、必需内容和职责边界提供可检查声明。 | 已确认飞书文档、同步 Markdown、执行结果和人工决策。 | 新会话能够定位当前阶段与下一步；关键入口缺失由 smoke 生存检查报告，完整文档治理问题由 lint 报告。 |
| 质量 Harness | 统一调用 clang-format、clang-tidy、CMake、Ninja、CTest，并执行模块结构、文档规范、链接有效性、状态职责和骨架文档同步检查。 | 源码骨架、测试、工具链、环境探测结果、文档规范和文档声明清单。 | 可重复验证结果；跳过项必须说明原因，失败不得被降级为成功。 |

典型控制流为：开发者或 Agent 调用 tools/ugdr；命令层读取 workspace 和环境状态；doctor 或 bootstrap 给出准备结果；format、lint、build、test、smoke 调用对应底层工具或检查规则；lint 负责完整静态一致性，smoke 负责确认仓库主路径没有阻断新会话继续工作的关键破损；最终以退出码、日志和状态记录形成可观察结果。

**已确认设计：** 使用 C++20 与 CUDA C++；使用 CMake/Ninja、CTest、clang-format、clang-tidy 和 Python 3；采用 include/ugdr、src/api、src/control、src/queues、src/worker、src/gpu、apps/client、apps/daemon、tests/unit、tests/integration、tests/smoke、tools 和 docs 分层；tools/ugdr 是稳定统一入口；docs/status/current.md、docs/decisions 和 docs/progress 分别承载当前状态、关键决策和执行进度；完整文档治理归入 lint，smoke 只做最小生存检查；骨架变更必须同步骨架说明文档并接受 lint 一致性检查。

**下沉到步骤文档：** 具体最低工具版本、安装命令、CMake target 名称、配置 schema、退出码枚举、脚本内部结构、CI 环境和每项检查的实现细节。v1 暂以当前本机开发环境为依据，不建立跨 Linux 主机的支持矩阵；步骤文档不得改变上述功能级边界；确需调整时先回到本功能文档确认。

## 七、步骤划分

将功能拆分为可独立设计、实现和验收的步骤。此处只定义步骤目标、交付、依赖和验收边界，不展开具体实现。

| 步骤标识 | 步骤名称 | 目标与交付 | 依赖 | 验收边界 |
|-|-|-|-|-|
| F01-S01 | 仓库骨架与模块依赖边界 | 建立已确认的目录分层、最小 CMake/CUDA 工程、占位 target、模块依赖说明、骨架说明文档和机械边界检查。 | UGDR_v1 版本文档；C++/CUDA 工具链。 | 干净 workspace 可完成配置与最小构建；允许依赖方向有文档说明；实际骨架与文档声明一致；构造的越界依赖能被检查拒绝。 |
| F01-S02 | 项目文档、状态与进度交接体系 | 建立精简 AGENTS.md、v1 文档入口、current 状态、决策记录、进度记录和文档治理规范，并定义更新责任。 | F01-S01；已确认飞书和 Markdown 文档。 | 所有入口链接有效；长期规则与可变状态分离；文档最大长度、必需内容和职责边界可由 lint 检查；新会话能找到当前阶段、下一步和验证入口。 |
| F01-S03 | Bootstrap 与环境诊断 | 实现 tools/ugdr bootstrap 和 doctor，探测主机工具链、Python、CMake/Ninja、clang、CUDA Toolkit 与 GPU。 | F01-S01；目标 Linux 环境。 | 重复执行结果稳定；缺失工具、版本不满足、CUDA 不可用和 GPU 不可用均有不同且可操作的诊断；不得伪造通过。具体最低版本和安装方式在步骤文档确认，v1 以本机环境为准。 |
| F01-S04 | 统一质量命令与基础门禁 | 实现 format、lint、build、test 和 smoke 子命令，接入 clang-format、clang-tidy、CMake/Ninja、CTest、结构检查、文档治理检查和 smoke 最小生存检查。 | F01-S01、F01-S03。 | 每个命令有稳定退出状态；正常路径可重复通过；格式、静态检查、构建、测试、结构、文档治理或骨架同步失败均能传播为非零结果；smoke 不承载完整文档治理。 |
| F01-S05 | 干净 workspace 与新 Agent 会话验收 | 在干净 workspace 和无历史聊天的新会话中执行必要 lint、完整 F01 smoke，记录结果、偏差和下一步。 | F01-S01 至 F01-S04。 | 新会话能从仓库恢复上下文并运行基础验证；全部必需门禁通过；环境限制和遗留问题被明确记录；人工验收后才进入 F02。 |

## 八、验证与功能验收标准

- 仓库具有与 F02-F07 边界一致的可编译骨架，公共接口、api、control、queues、worker、gpu、client、daemon 和测试分层清晰；模块依赖方向有文档说明并可机械检查；实际骨架与骨架说明文档声明一致。精简 AGENTS.md、v1 文档入口、当前状态、决策和进度载体均存在且链接有效，新 Agent 会话无需依赖历史聊天即可识别当前阶段和下一项工作。
- 在干净 workspace 中可通过 tools/ugdr 执行 bootstrap 或 doctor，并可执行 format、lint、build、test 和 smoke。命令具有稳定退出状态，CMake/Ninja 最小构建、CTest、格式检查、静态检查、模块边界检查、文档入口检查、文档长度与必需内容检查、状态职责边界检查和骨架文档同步检查均可重复运行。smoke 只覆盖新会话继续工作的最小生存路径。
- 缺少必需工具、工具版本不满足、CUDA Toolkit 不可用、GPU 不可用、构建失败、测试失败、格式或静态检查失败、越界依赖、失效文档链接、文档超出长度约束、文档职责混写、骨架与文档声明不一致均不得报告成功。允许跳过的检查必须输出原因和影响；bootstrap 无法自动处理的依赖必须给出人工修复方向。F01 未经人工验收不得进入 F02-F07 正式实现。

## 九、风险与待确认事项

| 类型 | 内容 | 影响 | 状态 |
|-|-|-|-|
| 已确认 | 采用 C++20、CUDA C++、CMake/Ninja、CTest、clang-format、clang-tidy、Python 3、既定目录分层和 tools/ugdr 统一入口。 | 作为 F01 各步骤的共同功能级约束。 | 已确认 |
| 已确认 | AGENTS.md 过大、文档链接失效，或规则、状态与进度混写会导致新会话获得错误上下文。 | 影响可交接性并引起实现偏离。 | 完整规则放入 lint；smoke 只跑最小生存检查；F01-S02、S04 实现。 |
| 已接受约束 | 不同 Linux 主机的编译器、CMake、CUDA Toolkit 和 GPU 能力不一致。 | 可能使 bootstrap、构建或 GPU 检查在跨环境场景下不可重复。 | v1 以当前本机环境为依据，不建立跨环境支持矩阵。 |
| 待步骤确认 | 各工具最低版本、依赖安装方式、CMake target 名称、退出码枚举和 CI 环境。 | 影响命令实现和后续可扩展性，但不改变功能目标与边界。 | 下沉到 F01-S03、S04 步骤文档确认。 |
| 已确认 | 仓库骨架过早固化后续运行时实现细节。 | 可能限制 F02-F07 的设计空间。 | 骨架只固定模块边界、最小占位和依赖方向；骨架变更必须同步骨架说明文档，并由 lint 检查实际骨架与文档声明一致；语义合理性由 skill 或人工审查。 |

## 十、变更记录

| 日期 | 变更内容 | 变更原因 | 影响范围 |
|-|-|-|-|
| 2026-07-18 | 基于 UGDR_v1 版本文档下沉并创建 F01 功能文档草稿，确认工具链、目录边界、状态载体、统一入口和五个步骤。 | 在进入 F02-F07 前建立人和 Agent 都能读取、执行、验证和持续交接的工程基础。 | F01-S01 至 F01-S05；后续全部功能的仓库与验证前置。 |
| 2026-07-18 | 确认文档治理归入 lint、smoke 只做最小生存检查、v1 暂以本机环境为依据、骨架变更必须同步骨架说明文档并由 lint 检查一致性。 | 细化已识别风险的处理策略，明确脚本硬性检查与 skill/人工审查的边界。 | 功能设计与模块边界；步骤划分；功能验收标准；风险与待确认事项。 |

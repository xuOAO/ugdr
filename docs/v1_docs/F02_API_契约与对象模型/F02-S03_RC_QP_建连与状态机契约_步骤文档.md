---
source_kind: "feishu-docx"
source_token: "PHDgdmRjYovDiTxnbNycuH1znHd"
source_url: "https://my.feishu.cn/docx/PHDgdmRjYovDiTxnbNycuH1znHd"
source_path: "我的空间 / UGDR / UGDR_v1 设计 / F02_API 契约与对象模型 / F02-S03_RC QP 建连与状态机契约 步骤文档"
source_title: "F02-S03_RC QP 建连与状态机契约 步骤文档"
source_revision: 19
doc_type: "step"
content_mode: "agent"
review_status: "reviewed"
synced_at: "2026-07-21T20:16:30+08:00"
generated_by: "ugdr-sync-docs-to-md"
generated_body_sha256: "4212139240fdc99895abae73781d7f5d9cbd0e7916801f836f8eaca0c86be833"
---
# F02-S03_RC QP 建连与状态机契约 步骤文档

**所属版本文档：** [UGDR_v1 版本文档](../UGDR_v1_版本文档.md)

**所属功能文档：** [F02_API 契约与对象模型 功能文档](F02_API_契约与对象模型_功能文档.md)

**所属版本：** v1

**功能标识：** F02-API 契约与对象模型

**步骤标识：** F02-S03-RC QP 建连与状态机契约

# 一、目标与完成条件

定义 UGDR v1 RC QP 的创建属性、连接信息、RESET→INIT→RTR→RTS 建连路径、查询语义、非法转换与失败原子性，并在公开 API 与 `docs/contracts/rc-qp-state-machine.md` 中固化可测试契约。完成时公开结构可编译，状态表、连接字段、错误结果和与 libibverbs 的对齐或扩展均可审阅；本步骤不实现 IPC、控制面对象或真实队列行为。

# 二、实现设计

## 2.1 已确认边界

- QP 在最终运行时创建成功后进入 `UGDR_QPS_RESET`；F02 期间公开入口仍是显式返回 `EOPNOTSUPP` 且不写输出的占位实现，不能以 Mock 冒充运行时。
- v1 只支持 RC。公开状态常量保留 RESET、INIT、RTR、RTS、SQD、SQE、ERR 与 UNKNOWN 的标准数值；可达正常路径只有 RESET→INIT→RTR→RTS，SQD 和 SQE 仅保留数值、不提供 v1 转换能力。
- `ugdr_modify_qp` 负责 RESET→INIT 和进入 ERR；`ugdr_connect_qp` 是 UGDR 扩展，在同一 daemon 控制域内原子完成 INIT→RTR→RTS。失败时不得暴露 RTR、部分 peer 绑定或其他中间状态。
- 连接信息只用于同一 daemon 控制域内的进程间交换；不定义网络字节序或持久化编码，不暴露 GID、LID、MTU、PSN、IP 或端口。
- ERR 下未完成 WR 的 flush、completion 以及复位交互由 F02-S04 定义；本步骤只固定进入 ERR 后不能继续建连或转回正常路径。

## 2.2 公开记录与掩码

| 公开项 | v1 字段或数值 | 约束 |
|-|-|-|
| `ugdr_qp_init_attr` | `send_cq`、`recv_cq`、`max_send_wr`、`max_recv_wr`、`max_send_sge`、`max_recv_sge`、`qp_type`、`sq_sig_all` | CQ 必须与 PD 同属一个 Context；容量和 SGE 上限必须非零；`qp_type` 仅接受 RC；`sq_sig_all` 仅接受 0 或 1。不暴露 SRQ 和 inline data。 |
| `ugdr_qp_attr` | `qp_state`、`cur_qp_state`、`qp_access_flags` | `qp_access_flags` 的 v1 QP 能力只允许 `UGDR_ACCESS_REMOTE_WRITE`；Local Write 是 MR 权限，不是 QP 转换属性。 |
| `ugdr_qp_attr_mask` | `UGDR_QP_STATE = 1U << 0U`、`UGDR_QP_CUR_STATE = 1U << 1U`、`UGDR_QP_ACCESS_FLAGS = 1U << 3U` | 数值与对应 `ibv_qp_attr_mask` 位对齐；未知位返回 `EINVAL`。 |
| `ugdr_qp_conn_info` | `uint32_t qp_num` | `qp_num` 是 daemon 控制域内的非零 QP 编号；同一 daemon 进程生命周期内全局分配且不复用。QP 销毁或 session 断连后，旧编号立即失效；daemon 重启后可重新分配。 |

上述字段顺序就是公开结构顺序。不得增加为硬件或网络建连服务的保留字段；未来扩展通过重新审阅 API 契约处理。

## 2.3 创建与查询契约

| 操作 | 成功结果 | 失败与原子性 |
|-|-|-|
| `ugdr_create_qp` | 校验 PD、两个 CQ、RC 类型、容量、SGE 上限和 `sq_sig_all`；创建的 QP 处于 RESET，并获得 daemon 控制域内全局分配的非零 `qp_num`。编号耗尽时创建失败且 `errno=ENOSPC`。 | 空指针、跨 Context 关联或非法字段返回空指针并令 `errno=EINVAL`；资源上限与实际容量策略留给运行时步骤，不在 F02 伪造成功。 |
| `ugdr_query_qp` | 按 mask 写入已请求的 state、current state 和 access flags，并返回创建时的 init attributes；若同时请求 state 和 current state，两者都是同一次快照的当前状态。 | 无效指针或未知 mask 位返回 `EINVAL`，所有输出保持不变。 |
| `ugdr_query_qp_conn_info` | 活 QP 在任一状态均可查询本地 `qp_num`；该信息不表示已连接。应用负责带外交换，v1 不定义交换通道或序列化。 | 无效或已销毁句柄返回 `EINVAL`，输出保持不变。 |

## 2.4 状态转换表

| 入口 | 起始状态 | 目标状态 | 必需条件 | 结果 |
|-|-|-|-|-|
| `ugdr_modify_qp` | RESET | INIT | mask 必须包含 STATE 和 ACCESS_FLAGS；access flags 必须且只能启用 REMOTE_WRITE。若带 CUR_STATE，其值必须等于 RESET。 | 成功进入 INIT；任一校验失败返回 `EINVAL` 且状态不变。 |
| `ugdr_connect_qp` | INIT | RTS | remote info 在同一 daemon 控制域解析到存活的 RC QP；远端状态为 INIT、RTR 或 RTS；本地未绑定其他 peer。 | 以单次可观察提交完成 INIT→RTR→RTS，并记录 peer。 |
| `ugdr_modify_qp` | RESET、INIT、RTR 或 RTS | ERR | mask 包含 STATE；若带 CUR_STATE，其值等于调用时状态；除状态 guard 外不得携带无关字段。 | 成功进入 ERR；队列 flush 与 WC 结果留给 F02-S04。 |
| `ugdr_modify_qp` | 任意 | SQD 或 SQE | 无 | 返回 `EOPNOTSUPP`，状态不变。 |
| 任一转换入口 | 其他组合 | 任意 | 不满足上表 | 返回 `EINVAL`，状态、peer 绑定及输出不变。 |

ERR 是 v1 的终止状态。ERR→RESET、ERR→INIT 以及显式 RTR 或 RTS 修改均不属于本步骤支持集合。对已经绑定的 QP 传入不同 peer 时，`ugdr_connect_qp` 返回 `EBUSY`；对同一 peer 重复调用不视为幂等建连，而是因本地已不在 INIT 返回 `EINVAL`。

## 2.5 原子建连流程

```python
def connect(local_qp, remote_info):
    validate_input_without_writing_state()
    if local_qp.is_bound_to_different_peer(remote_info):
        return EBUSY
    require(local_qp.state == INIT, EINVAL)
    remote_qp = resolve_in_same_daemon(remote_info.qp_num)
    require(remote_qp exists, ENOENT)
    require(remote_qp.type == RC and remote_qp.state in {INIT, RTR, RTS}, EINVAL)

    staged_peer = remote_qp.identity
    staged_state = apply_INIT_to_RTR_to_RTS_offline()
    commit_peer_and_state_once(staged_peer, staged_state)
    return 0
```

远端 `qp_num` 未知、已销毁或不属于当前 daemon 控制域时返回 `ENOENT`。验证和 staged transition 任一步失败都不能改变本地 QP、远端 QP 或调用者输出；函数不隐式推进远端状态。

## 2.6 错误优先级与占位期行为

| 条件 | 结果 |
|-|-|
| 无效句柄、空必需参数、非法字段、未知 mask、非法正常转换或状态 guard 不匹配 | `EINVAL` |
| remote `qp_num` 未知、已销毁或不属于当前 daemon 控制域 | `ENOENT` |
| 本地已绑定到不同 peer | `EBUSY` |
| 请求进入 SQD 或 SQE | `EOPNOTSUPP` |
| F02 尚无运行时实现的公开入口 | 保持现有 `EOPNOTSUPP` 占位结果；指针返回入口同时设置 `errno`，查询不得写输出。 |

执行错误判定时先验证本地参数和句柄，再判断既有 peer 冲突，再解析 remote info，最后验证状态组合；专项负向测试固定这一优先级，避免同一输入因实现细节返回不同错误。

## 2.7 文件与任务

| 任务 | 改动 | 依赖 |
|-|-|-|
| T01 公开 API 形状 | 在 `include/ugdr/api.hpp` 定义三个公开记录与 attr mask；扩展 `tests/unit/api_contract_test.cpp` 固定字段类型、顺序、数值以及占位入口不写输出。 | 无 |
| T02 状态与连接契约 | 新增 `docs/contracts/rc-qp-state-machine.md`，写入创建、查询、状态矩阵、连接流程、错误优先级和负向用例；新增 `docs/decisions/0003-atomic-rc-connect-helper.md` 记录 UGDR 原子建连扩展。 | T01 |
| T03 对齐与索引 | 更新 `docs/contracts/public-api.md`、`docs/contracts/libibverbs-alignment.md` 与 `docs/contracts/README.md`，消除 F02-S03 待定项；不提前修改由 F02-S05 负责的 AGENTS.md 文档地图。 | T02 |
| T04 验证与交接 | 运行编译、测试和文档治理检查，记录实现证据；人工验收前不勾选“已实现”。 | T03 |

```mermaid
flowchart LR
    T01[T01 公开 API 形状] --> T02[T02 状态与连接契约]
    T02 --> T03[T03 对齐与索引]
    T03 --> T04[T04 验证与交接]
```

# 三、验证与验收

本步骤的验收同时覆盖公开 API 形状、契约完整性、占位期负向行为和范围边界。实现完成后按下表执行；任何命令失败、输出被部分写入、状态矩阵存在空白，或文档引入网络和内部实现细节，都视为未完成。

| 验证项 | 方式 | 预期结果 |
|-|-|-|
| 公开结构与数值 | 构建并运行 `api_contract_test`；静态断言字段类型、顺序、attr mask 数值、RC 与 QP state 数值。 | 编译和测试通过；Client 不需要 daemon、GPU 或 RDMA 设备。 |
| 占位入口负向行为 | 对 create、modify、query、query connection info 与 connect 使用 sentinel 输出执行现有占位测试。 | 统一显式报告 `EOPNOTSUPP`，不返回成功、不写输出、不产生状态。 |
| 状态与错误矩阵 | 逐行审计 `docs/contracts/rc-qp-state-machine.md`，覆盖 RESET→INIT、原子 connect、进入 ERR、SQD/SQE、非法转换、已销毁 `qp_num` 与 peer 冲突。 | 每个合法和非法组合都有唯一结果；失败原子性和错误优先级明确。 |
| RDMA 语义与范围 | 对照 `docs/contracts/libibverbs-alignment.md`，检查标准数值、术语和 UGDR 扩展决策；搜索 GID、LID、MTU、PSN、IP、port、IPC encoding 等越界承诺。 | 标准对齐项与扩展项均有追踪；不宣称网络建连、序列化或完整 verbs 支持。 |
| 仓库验证 | `cmake --build build`；`ctest --test-dir build --output-on-failure`；`python3 tools/project_state.py validate --root .`；`python3 tools/check_project_docs.py --root .`；`git diff --check` | 全部通过；执行证据写入本步骤 progress 记录。 |

测试通过不能替代人工实现验收；后续实现与验收完成后，再由人工勾选飞书文档中的“已实现”。

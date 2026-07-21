---
source_kind: "feishu-docx"
source_token: "Cae4dsPhQoO8k8x85NTcId9InDb"
source_url: "https://my.feishu.cn/docx/Cae4dsPhQoO8k8x85NTcId9InDb"
source_path: "我的空间 / UGDR / UGDR_v1 设计 / F03_Daemon 控制面与对象生命周期 / F03-S01_IPC 协议与 session 生命周期 步骤文档"
source_title: "F03-S01_IPC 协议与 session 生命周期 步骤文档"
source_revision: 15
doc_type: "step"
content_mode: "agent"
review_status: "reviewed"
synced_at: "2026-07-21T13:06:05+08:00"
generated_by: "ugdr-sync-docs-to-md"
generated_body_sha256: "a208531b849f2c94f55dffbc0147cf771765acea1e9da78fc7307dd176638294"
---
# F03-S01_IPC 协议与 session 生命周期 步骤文档

所属版本：UGDR_v1

所属版本文档：[UGDR_v1 版本文档](../UGDR_v1_版本文档.md)

所属功能文档：[F03_Daemon 控制面与对象生命周期 功能文档](F03_Daemon_控制面与对象生命周期_功能文档.md)

## 一、目标与完成条件

实现一个与 RDMA/UGDR 业务无关的本机 IPC 组件，并通过 adapter 将通用 IPC 消息转换为 UGDR 控制语义；daemon 使用一个控制线程运行 IPC server。本步骤只建立通信、类型封装和序列化边界，不创建 Context、PD、MR、CQ、QP、SQ 或 RQ 等真实资源。

完成时，独立进程间可以可靠传递普通信息和一个或多个 fd，UGDR 控制请求与响应可以在类型化对象和 IPC 消息之间往返转换，daemon 可以单线程接受连接、分派消息并在断连时结束对应 session。

## 二、实现设计

### 1. 通用 IPC 协议设计

通用 IPC 组件使用 Unix Domain `SOCK_SEQPACKET`，一条 seqpacket 对应一条完整消息。协议只定义通用 envelope、字节 payload 和 fd 附件，不出现 MR、QP、SQ、RQ、CUDA 或其他 UGDR/RDMA 术语。

| 组成 | 约束 |
|-|-|
| Envelope | 包含 magic、协议版本、method、flags、request_id、status、payload_length 和 fd_count；整数采用固定宽度和明确字节序逐字段编码，不直接发送 C/C++ 对象内存。 |
| Inline payload | 承载普通字节数据；CUDA IPC handle 等不透明数据按字节传输。 |
| FD attachments | 通过 `sendmsg/recvmsg` 和 `SCM_RIGHTS` 传输；payload 只引用 fd 在附件列表中的索引，不传递发送进程内的 fd 数值。 |
| 错误处理 | 长度、版本、flags、fd_count、fd 索引、`MSG_TRUNC` 或 `MSG_CTRUNC` 校验失败时拒绝消息，并关闭本次已经接收但未移交的 fd。 |

协议不定义 HELLO、PING 或 GOODBYE。版本随每条消息检查；连接 EOF 或 reset 直接表示该 IPC session 结束。发送方保留原 fd 的所有权，接收方获得并负责关闭由 `SCM_RIGHTS` 创建的 fd。

### 2. IPC server/client 类型封装

| 类型 | 职责 |
|-|-|
| `IpcMessage` | 保存 envelope、inline payload 和具有唯一所有权的 fd 列表。 |
| `IpcClient` | 同步完成 connect、request/response call 与 close；负责 request_id、消息编解码和 fd 收发，不启动后台线程。 |
| `IpcServer` | 负责 listen、accept、连接到 session 的映射、消息收发和断连通知；通过注入的 handler 分派消息，不解释业务 payload。 |
| `IpcHandler` | 接收 session 与 `IpcMessage`，返回响应；使通用组件可被 UGDR adapter 或测试 handler 复用。 |

socket、连接和接收 fd 使用 RAII 所有权。消息只有在 envelope、payload 和全部 fd 验证通过后才交给 handler；失败路径不得留下半移交 fd 或半分派请求。

### 3. Adapter：IPC 协议与 UGDR 控制语义转换

Adapter 是唯一理解 UGDR 控制语义的层。它将类型化的 `UgdrControlRequest`/`UgdrControlResponse` 编码为通用 `IpcMessage`，并执行反向解码；通用 IPC 组件不依赖 adapter。

UGDR 对象 identity、属性、长度、access、opaque CUDA IPC handle 和队列 fd 索引都必须按字段显式编码。CP/SQ/RQ 等跨进程资源由类型化请求记录 fd 索引，真实 fd 保存在 `IpcMessage` 的附件列表中。Adapter 校验 method、payload 形状、fd 数量和索引后才生成类型化对象，禁止传递进程指针、裸 C++ 对象布局或未经校验的 fd 数值。

本步骤只实现 adapter 的类型边界以及请求/响应的序列化和反序列化，不调用 Context 或对象注册表，不创建 PD 等资源，也不打开 CUDA mapping。后续步骤只需为相应 UGDR 请求接入业务 handler。

### 4. Daemon 单线程使用 IPC server

daemon 在一个控制线程中运行 `IpcServer`，使用 `poll()` 同时处理 listener 和全部 client fd。新连接建立 session，消息到达后经 adapter 解码并交给控制 handler；EOF/reset 触发一次 session 断连通知。这里不采用每连接一个线程、`std::jthread`、session worker 或为 session map 加锁。

设计伪代码：

```python
while running:
    ready_fds = poll(listener_fd, client_fds)
    for fd in ready_fds:
        if fd == listener_fd:
            accept_and_create_session()
        else:
            message = recv_ipc_message(fd)
            if message is EOF:
                close_session_once(fd)
                continue
            request = ugdr_adapter.decode_request(message)
            response = control_handler(request)
            send_ipc_message(fd, ugdr_adapter.encode_response(response))
```

daemon 停止时关闭 listener 与现有连接，并只清理由本实例创建的 socket path。S01 的 daemon 接入用于证明 server、adapter 和单线程循环可以组合，不要求控制 handler 创建任何 verbs 资源。

### 5. 预计新增/修改文件

下表按当前仓库骨架列出本步骤预计涉及的文件。S01 不修改公开 API 文件，也不把测试协议写入生产代码。

| 文件 | 动作 | 职责 |
|-|-|-|
| `src/ipc/ipc.hpp` | 新增 | 声明通用 `IpcMessage`、fd 所有权类型、`IpcClient`、`IpcServer` 和 handler 接口。 |
| `src/ipc/protocol.cpp` | 新增 | 实现 envelope 与 inline payload 的逐字段编解码、消息校验以及 `SCM_RIGHTS` fd 附件收发。 |
| `src/ipc/client.cpp` | 新增 | 实现同步 connect、request/response call 和 close。 |
| `src/ipc/server.cpp` | 新增 | 实现 listen、accept、单线程 `poll()`、session 映射、handler 分派和断连通知。 |
| `src/control/ipc_adapter.hpp`、`src/control/ipc_adapter.cpp` | 新增 | 定义类型化 UGDR 控制 request/response，并在它们与通用 `IpcMessage` 之间序列化和反序列化。 |
| `src/control/control.hpp`、`src/control/control.cpp` | 修改 | 替换控制面占位接口，提供 daemon 组合 adapter 所需的控制 handler 与 session 断连入口；S01 不创建真实资源。 |
| `apps/daemon/main.cpp` | 修改 | 组合 `IpcServer`、UGDR adapter 和控制 handler，并以单线程运行控制循环。 |
| `tests/unit/ipc_protocol_test.cpp`、`tests/unit/ipc_adapter_test.cpp` | 新增 | 分别验证通用消息编解码/fd 边界，以及 UGDR 类型化语义 round-trip。 |
| `tests/integration/ipc_client_server_test.cpp` | 新增 | 用独立 client/server 进程验证 inline payload、零个或多个 fd、请求响应关联与断连。 |
| `CMakeLists.txt`、`tests/unit/CMakeLists.txt`、`tests/integration/CMakeLists.txt` | 修改 | 登记 `ugdr_ipc` 及新增测试，并连接 daemon/control 所需依赖。 |
| `tools/module-boundaries.json`、`docs/architecture/repository-skeleton.md` | 修改 | 登记 `src/ipc`、`ugdr_ipc` 及允许依赖，并同步机器策略与生成架构表。 |

## 三、验证与验收

| 验证动作 | 预期结果 | 失败判定 |
|-|-|-|
| IPC 信息与 fd 往返 | 独立 client/server 进程能够传递 inline payload 以及零个、一个或多个测试 fd；接收端可正常使用 fd，request/response 关联正确，daemon 由单线程处理连接和消息。 | 信息损坏、fd 不可用或错配、跨连接串包、截断未被拒绝、错误路径泄漏 fd，或需要每连接线程才能完成。 |
| UGDR adapter round-trip | 覆盖标量、对象 identity、不透明字节和 fd 索引的代表性 UGDR request/response，满足类型化对象 → IPC 消息 → 类型化对象逐字段一致；非法 payload、fd_count 或索引被拒绝。 | 依赖 C/C++ 对象内存布局、字段往返不一致、非法输入被接受，或测试必须创建 PD/MR/QP 等真实资源才能通过。 |

验收不要求 RDMA 设备、CUDA 设备或真实对象注册表；只验证 IPC 通道、fd 所有权、adapter 编解码和 daemon 单线程接入。

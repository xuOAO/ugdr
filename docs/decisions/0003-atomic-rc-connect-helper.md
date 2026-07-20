# Atomic Same-Daemon RC Connect Helper

## Status

Accepted on 2026-07-20 by the reviewed
[F02-S03 RC QP state-machine design](../v1_docs/F02_API_契约与对象模型/F02-S03_RC_QP_建连与状态机契约_步骤文档.md),
Feishu revision 7.

## Context

Standard RC setup exposes INIT to RTR and RTR to RTS as separate `ibv_modify_qp` operations with
hardware and network path attributes. UGDR v1 supports only a same-machine, same-daemon control
domain and deliberately has no public GID, LID, MTU, PSN, IP address, port, retry, or wire-format
contract. Requiring Clients to manufacture those fields would falsely claim network verbs support,
while exposing an intermediate RTR state across IPC would add no useful v1 capability.

## Decision

Provide `ugdr_connect_qp` as a UGDR extension:

- The local QP must start in INIT.
- `ugdr_qp_conn_info` contains only a standard-style `qp_num` and an opaque, generation-safe
  `endpoint_id`.
- The remote identity must resolve in the same daemon to a live RC QP in INIT, RTR, or RTS.
- The implementation stages INIT to RTR and RTR to RTS, then commits the peer binding and RTS state
  once.
- Failure exposes no RTR state, partial peer binding, remote state change, or output modification.
- Unknown or stale endpoints report `ENOENT`; a different existing peer reports `EBUSY`; invalid
  fields or state combinations report `EINVAL`.
- SQD and SQE numeric constants remain aligned but requests to enter them report `EOPNOTSUPP`.

The helper does not define a serialized connection record and does not advance the remote QP. Each
side connects independently.

## Consequences

- Client code has a compact v1 connection path without depending on unsupported network attributes.
- Daemon implementations must support generation-safe endpoint resolution and a transactional local
  state/peer commit.
- Tests can assert that every failure preserves state and that success becomes directly observable
  as RTS.
- `ugdr_connect_qp` remains explicitly classified as a UGDR extension, not as ABI compatibility
  with one libibverbs call.
- Hardware/network RC setup, cross-daemon connection, serialization, queue behavior, and ERR flush
  completion remain outside this decision.

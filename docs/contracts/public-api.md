# v1 Public API

Source: [reviewed F02-S01 revision 16](../v1_docs/F02_API_契约与对象模型/F02-S01_v1_公开_API_表面与对齐基线_步骤文档.md).

`include/ugdr/api.hpp` exposes C-linkage `ugdr_*` symbols and C-compatible declarations. F02-S01
freezes the symbol and type names needed by the v1 Client. Records whose fields belong to later F02
steps remain incomplete types: Client code may name and pass their pointers, but must not allocate,
inspect, or depend on their layout yet.

## Types

| Category | Public names | F02-S01 contract |
|-|-|-|
| Resource handles | `ugdr_device`, `ugdr_context`, `ugdr_pd`, `ugdr_mr`, `ugdr_cq`, `ugdr_qp` | Opaque records. Lifecycle and ownership are deferred to F02-S02. |
| Optional CQ event channel | `ugdr_comp_channel` | Opaque signature-alignment type. Event channels are unsupported in v1; callers pass null and use completion vector 0. |
| QP attributes | `ugdr_qp_init_attr`, `ugdr_qp_attr`, `ugdr_qp_conn_info` | Incomplete records. Fields are deferred to F02-S02/S03. |
| Work requests | `ugdr_sge`, `ugdr_send_wr`, `ugdr_recv_wr` | Incomplete records. Fields and posting semantics are deferred to F02-S04. |
| Completion | `ugdr_wc` | Incomplete record. Fields and completion semantics are deferred to F02-S04. |
| QP type | `ugdr_qp_type` | RC only; `UGDR_QPT_RC` is numerically aligned with `IBV_QPT_RC`. |
| QP state | `ugdr_qp_state` | Names and values align with `IBV_QPS_*`; transition rules are deferred to F02-S03. |
| WR opcode | `ugdr_wr_opcode` | Only RDMA Write and RDMA Write With Immediate are exposed. |
| Send flags | `ugdr_send_flags` | Only `UGDR_SEND_SIGNALED` is exposed in this baseline. |
| WC status/opcode | `ugdr_wc_status`, `ugdr_wc_opcode` | The v1 subset uses libibverbs numeric values; exact generation rules are deferred to F02-S04. |
| MR access | `ugdr_access_flags` | Only local-write and remote-write flags are exposed. |

## Functions and placeholder results

All functions are linkable in F02-S01. No function creates runtime state, consumes a WR, produces a
WC, writes an output record, or returns a fake handle.

| Function group | Public functions | F02-S01 placeholder result |
|-|-|-|
| Device list | `ugdr_get_device_list`, `ugdr_free_device_list` | Get returns null and sets `errno=EOPNOTSUPP` without changing `num_devices`. Free has no return channel, performs no work, and sets `errno=EOPNOTSUPP`. |
| Context | `ugdr_open_device`, `ugdr_close_device` | Open returns null and sets `errno=EOPNOTSUPP`; close returns `-1` and sets `errno=EOPNOTSUPP`. |
| PD | `ugdr_alloc_pd`, `ugdr_dealloc_pd` | Allocate returns null and sets `errno=EOPNOTSUPP`; deallocate returns `EOPNOTSUPP`. |
| MR | `ugdr_reg_mr`, `ugdr_dereg_mr` | Register returns null and sets `errno=EOPNOTSUPP`; deregister returns `EOPNOTSUPP`. |
| CQ | `ugdr_create_cq`, `ugdr_destroy_cq`, `ugdr_poll_cq` | Create returns null and sets `errno=EOPNOTSUPP`; destroy returns `EOPNOTSUPP`; poll returns `-EOPNOTSUPP` and does not write `wc`. |
| QP | `ugdr_create_qp`, `ugdr_destroy_qp`, `ugdr_modify_qp`, `ugdr_query_qp` | Create returns null and sets `errno=EOPNOTSUPP`; the integer operations return `EOPNOTSUPP` and do not change inputs or outputs. |
| Connection extension | `ugdr_query_qp_conn_info`, `ugdr_connect_qp` | Return `EOPNOTSUPP`; query does not write connection information. |
| WR posting | `ugdr_post_send`, `ugdr_post_recv` | Return `EOPNOTSUPP`; do not consume any WR and do not change `bad_wr`. |

Except for pointer-returning functions, `ugdr_close_device`, the negative error domain of
`ugdr_poll_cq`, and the void `ugdr_free_device_list`, integer APIs return an errno value directly as
their corresponding libibverbs APIs do.

## Explicit non-capabilities

This baseline does not implement device discovery, IPC, daemon sessions, object lifecycle, QP
transitions, queue storage, WR consumption, WC production, worker execution, or GPU data movement.
A successful placeholder result would violate this contract.

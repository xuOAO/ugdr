# v1 Public API

Sources:

- [reviewed F02-S01 revision 16](../v1_docs/F02_API_契约与对象模型/F02-S01_v1_公开_API_表面与对齐基线_步骤文档.md)
- [reviewed F02-S03 revision 7](../v1_docs/F02_API_契约与对象模型/F02-S03_RC_QP_建连与状态机契约_步骤文档.md)
- [reviewed F02-S04 revision 20](../v1_docs/F02_API_契约与对象模型/F02-S04_WR_WC_与完成语义契约_步骤文档.md)

`include/ugdr/api.hpp` exposes C-linkage `ugdr_*` symbols and C-compatible declarations. F02-S01
freezes the symbol and type names needed by the v1 Client; F02-S03 defines the initial public QP
records; F02-S04 completes the MR, SGE, WR, WC, retry-attribute, and completion surface.

## Types

| Category | Public names | F02-S01 contract |
|-|-|-|
| Opaque resource handles | `ugdr_device`, `ugdr_context`, `ugdr_pd`, `ugdr_cq`, `ugdr_qp` | Opaque records with the reviewed child-first lifecycle. |
| Memory region | `ugdr_mr` | Public standard-style record containing `context`, `pd`, `addr`, `length`, `handle`, `lkey`, and `rkey`; callers directly read `mr->lkey` and `mr->rkey`. |
| Optional CQ event channel | `ugdr_comp_channel` | Opaque signature-alignment type. Event channels are unsupported in v1; callers pass null and use completion vector 0. |
| QP creation attributes | `ugdr_qp_init_attr` | Complete C-compatible record: send/receive CQ, SQ/RQ WR capacities, Send/Receive SGE maxima, RC type, and `sq_sig_all`. No SRQ or inline-data field. |
| QP state attributes | `ugdr_qp_attr`, `ugdr_qp_attr_mask` | Subset-adapted state/current-state/access/retry record. Supported mask bits 0, 1, 3, 9, 10, 11, and 15 use libibverbs values. |
| QP connection identity | `ugdr_qp_conn_info` | Complete same-daemon record containing `uint32_t qp_num` and generation-safe `uint64_t endpoint_id`; not a serialized network record. |
| Work requests | `ugdr_sge`, `ugdr_send_wr`, `ugdr_recv_wr` | Complete v1 records. SGE and Receive WR match the standard shape; Send WR preserves the standard relevant prefix, anonymous `imm_data`, and `wr.rdma` access path while omitting unsupported opcode unions. |
| Completion | `ugdr_wc` | Standard relevant base WC shape with unsupported invalidated-rkey access omitted; non-success validity, production, ordering, and polling rules are fixed by the [WR/WC contract](wr-wc-semantics.md). |
| QP type | `ugdr_qp_type` | RC only; `UGDR_QPT_RC` is numerically aligned with `IBV_QPT_RC`. |
| QP state | `ugdr_qp_state` | Names and values align with `IBV_QPS_*`; the v1 supported transitions are fixed by the [RC QP state contract](rc-qp-state-machine.md). |
| WR opcode | `ugdr_wr_opcode` | Only RDMA Write and RDMA Write With Immediate are exposed. |
| Send flags | `ugdr_send_flags` | Only `UGDR_SEND_SIGNALED` is exposed in this baseline. |
| WC status/opcode/flags | `ugdr_wc_status`, `ugdr_wc_opcode`, `ugdr_wc_flags` | The v1 subset uses libibverbs numeric values. The only exposed WC flag is `UGDR_WC_WITH_IMM`. |
| MR access | `ugdr_access_flags` | Only local-write and remote-write flags are exposed. |

## RC QP records and observable state

`ugdr_qp_init_attr` fixes the public field order as `send_cq`, `recv_cq`, `max_send_wr`,
`max_recv_wr`, `max_send_sge`, `max_recv_sge`, `qp_type`, and `sq_sig_all`.
`ugdr_qp_attr` fixes `qp_state`, `cur_qp_state`, `qp_access_flags`, `timeout`, `retry_cnt`,
`rnr_retry`, and `min_rnr_timer`.
`ugdr_qp_conn_info` fixes `qp_num` followed by `endpoint_id`.

The normal v1 path is RESET to INIT through `ugdr_modify_qp`, followed by the UGDR
`ugdr_connect_qp` helper's atomic INIT to RTR to RTS staging and commit. RESET, INIT, RTR, and RTS
may enter ERR through `ugdr_modify_qp`. SQD and SQE values remain public for numeric alignment, but
requests to enter them return `EOPNOTSUPP`. Exact masks, errors, connection-domain rules, query
semantics, and failure atomicity are defined in [RC QP State Machine](rc-qp-state-machine.md).
`ugdr_connect_qp` takes the remote identity plus a const attribute record and mask; it requires
`UGDR_QP_TIMEOUT`, `UGDR_QP_RETRY_CNT`, `UGDR_QP_RNR_RETRY`, and `UGDR_QP_MIN_RNR_TIMER`.

`ugdr_mr`, `ugdr_sge`, `ugdr_send_wr`, `ugdr_recv_wr`, and `ugdr_wc` use the exact public fields
listed in [WR/WC and Completion Semantics](wr-wc-semantics.md). In particular, Client code uses
`mr->lkey`, `mr->rkey`, `wr.imm_data`, `wr.wr.rdma.remote_addr`, `wr.wr.rdma.rkey`, and
`wc.imm_data` without UGDR-specific getters or alternate nesting.

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
| Connection extension | `ugdr_query_qp_conn_info`, `ugdr_connect_qp` | Return `EOPNOTSUPP`; query does not write connection information, and connect does not modify the remote record or retry attributes. |
| WR posting | `ugdr_post_send`, `ugdr_post_recv` | Return `EOPNOTSUPP`; do not consume any WR and do not change `bad_wr`. |

Except for pointer-returning functions, `ugdr_close_device`, the negative error domain of
`ugdr_poll_cq`, and the void `ugdr_free_device_list`, integer APIs return an errno value directly as
their corresponding libibverbs APIs do.

## Explicit non-capabilities

This F02 surface does not implement device discovery, IPC, daemon sessions, object lifecycle, QP
transitions, endpoint resolution, queue storage, WR consumption, WC production, worker execution,
network transport, or GPU data movement. A successful placeholder result would violate this
contract.

# v1 Public API

Sources:

- [reviewed F02-S01 revision 16](../v1_docs/F02_API_契约与对象模型/F02-S01_v1_公开_API_表面与对齐基线_步骤文档.md)
- [reviewed F02-S03 revision 7](../v1_docs/F02_API_契约与对象模型/F02-S03_RC_QP_建连与状态机契约_步骤文档.md)
- [reviewed F02-S04 revision 20](../v1_docs/F02_API_契约与对象模型/F02-S04_WR_WC_与完成语义契约_步骤文档.md)
- [reviewed F03-S02 revision 11](../v1_docs/F03_Daemon_控制面与对象生命周期/F03-S02_类型化_generation_handle_注册表与_Context_步骤文档.md)
- [reviewed F03-S03 revision 13](../v1_docs/F03_Daemon_控制面与对象生命周期/F03-S03_PD、MR、CQ_元数据与严格生命周期_步骤文档.md)
- [reviewed F03-S04 revision 7](../v1_docs/F03_Daemon_控制面与对象生命周期/F03-S04_QP、SQ、RQ_所有权与_CQ_关联_步骤文档.md)

`include/ugdr/api.hpp` exposes C-linkage `ugdr_*` symbols and C-compatible declarations. F02-S01
freezes the symbol and type names needed by the v1 Client; F02-S03 defines the initial public QP
records; F02-S04 completes the MR, SGE, WR, WC, retry-attribute, and completion surface. F03-S03
implements PD, CUDA-backed MR, and CQ control lifecycles, and F03-S04 implements QP creation and
destruction without changing that public ABI.

## Types

| Category | Public names | F02-S01 contract |
|-|-|-|
| Opaque resource handles | `ugdr_device`, `ugdr_context`, `ugdr_pd`, `ugdr_cq`, `ugdr_qp` | Opaque records with the reviewed child-first lifecycle. |
| Memory region | `ugdr_mr` | Public standard-style record containing `context`, `pd`, `addr`, `length`, `handle`, `lkey`, and `rkey`; callers directly read `mr->lkey` and `mr->rkey`. |
| Optional CQ event channel | `ugdr_comp_channel` | Opaque signature-alignment type. Event channels are unsupported in v1; callers pass null and use completion vector 0. |
| QP creation attributes | `ugdr_qp_init_attr` | Complete C-compatible record: send/receive CQ, SQ/RQ WR capacities, Send/Receive SGE maxima, RC type, and `sq_sig_all`. No SRQ or inline-data field. |
| QP state attributes | `ugdr_qp_attr`, `ugdr_qp_attr_mask` | Subset-adapted state/current-state/access/retry record. Supported mask bits 0, 1, 3, 9, 10, 11, and 15 use libibverbs values. |
| QP connection identity | `ugdr_qp_conn_info` | Same-daemon record containing only nonzero `uint32_t qp_num`; not a serialized network record. |
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
`ugdr_qp_conn_info` contains only `qp_num`.

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

## Functions and current results

All functions remain linkable. F03-S04 adds QP creation and destruction to the Device, Context, PD,
CUDA-backed MR, and CQ control lifecycles already implemented through the daemon. QP state,
connection, and data-path entry points retain their reviewed placeholders.

| Function group | Public functions | Current result |
|-|-|-|
| Device list | `ugdr_get_device_list`, `ugdr_free_device_list` | Get returns a null-terminated daemon enumeration and writes `num_devices` only on success. Transport or protocol failure returns null with `errno`. Free invalidates that list's Device proxies; invalid or repeated free sets `errno=EINVAL`. |
| Context | `ugdr_open_device`, `ugdr_close_device` | Open creates a session-owned daemon Context from a live Device. Close returns 0 on success; invalid/stale/repeated handles return `-1` with `errno=EINVAL`, while live children produce `EBUSY` without state change. |
| PD | `ugdr_alloc_pd`, `ugdr_dealloc_pd` | Allocate creates a Context child. Deallocate returns 0 only when no MR exists; live children return `EBUSY`, while invalid, stale, or repeated handles return `EINVAL`. |
| MR | `ugdr_reg_mr`, `ugdr_dereg_mr` | Register accepts a nonempty range inside a `cudaMalloc` device allocation, returns the Client address snapshot and direct nonzero `lkey`/`rkey`, and reports pointer failures through `errno`. Remote Write requires Local Write. Host, managed, array, VMM, or otherwise unsupported memory returns `EOPNOTSUPP`; malformed ranges and access return `EINVAL`. Deregister closes the daemon IPC mapping before invalidating the handle and keys. |
| CQ | `ugdr_create_cq`, `ugdr_destroy_cq`, `ugdr_poll_cq` | Create requires `cqe > 0`, null channel, and completion vector 0. Destroy enforces strict references. Poll on a live CQ remains `-EOPNOTSUPP` and does not write `wc`; invalid CQ handles return `-EINVAL`. |
| QP | `ugdr_create_qp`, `ugdr_destroy_qp`, `ugdr_modify_qp`, `ugdr_query_qp` | Create returns a RESET RC QP with a daemon-lifetime-unique QPN. Modify supports RESET→INIT and RESET/INIT/RTR/RTS→ERR. Query returns one state/access/retry snapshot plus creation attributes. Failures preserve state and outputs. |
| Connection extension | `ugdr_query_qp_conn_info`, `ugdr_connect_qp` | Query returns the local QPN. Connect resolves a live same-daemon remote QPN and atomically commits the local peer, retry fields, and RTS state; it never modifies the remote QP. |
| WR posting | `ugdr_post_send`, `ugdr_post_recv` | Return `EOPNOTSUPP`; do not consume any WR and do not change `bad_wr`. |

Except for pointer-returning functions, `ugdr_close_device`, the negative error domain of
`ugdr_poll_cq`, and the void `ugdr_free_device_list`, integer APIs return an errno value directly as
their corresponding libibverbs APIs do.

## Explicit non-capabilities

F03-S04 stores SQ/RQ capacities as QP-owned metadata but does not allocate queue storage, accept
WRs, produce WCs, run a worker, or move application payloads. QP transitions, endpoint resolution,
WR consumption, WC production, and network transport remain placeholders until their owning
reviewed steps.

# libibverbs Alignment

Sources:

- [reviewed F02-S01 revision 16](../v1_docs/F02_API_契约与对象模型/F02-S01_v1_公开_API_表面与对齐基线_步骤文档.md)
- [reviewed F02-S02 revision 17](../v1_docs/F02_API_契约与对象模型/F02-S02_对象模型与生命周期契约_步骤文档.md)
- [reviewed F02-S03 revision 7](../v1_docs/F02_API_契约与对象模型/F02-S03_RC_QP_建连与状态机契约_步骤文档.md)
- [reviewed F02-S04 revision 20](../v1_docs/F02_API_契约与对象模型/F02-S04_WR_WC_与完成语义契约_步骤文档.md)
- [reviewed F03-S03 revision 13](../v1_docs/F03_Daemon_控制面与对象生命周期/F03-S03_PD、MR、CQ_元数据与严格生命周期_步骤文档.md)
- [reviewed F03-S04 revision 7](../v1_docs/F03_Daemon_控制面与对象生命周期/F03-S04_QP、SQ、RQ_所有权与_CQ_关联_步骤文档.md)
- [reviewed F04-S01 revision 12](../v1_docs/F04_SQ、RQ、CQ_队列系统/F04-S01_共享队列与映射边界_步骤文档.md)
- [reviewed F04-S02 revision 9](../v1_docs/F04_SQ、RQ、CQ_队列系统/F04-S02_SQ、RQ_posting_快路径_步骤文档.md)

Status meanings:

- **aligned**: the supported portion has the same public shape, field types and order, access path,
  return domain, numeric constants, and observable semantics as libibverbs.
- **subset adaptation**: a reviewed projection, flattening, or omission keeps individually listed
  fields or values aligned but is not the complete corresponding verbs record or behavior.
- **UGDR extension**: UGDR-specific Client helper with no single verbs equivalent.
- **UGDR strict guarantee**: libibverbs-aligned relationship with a stronger deterministic UGDR
  failure or atomicity guarantee.
- **unsupported**: intentionally outside the v1 subset.

## Type alignment

| UGDR public item | libibverbs item | Status | Notes |
|-|-|-|-|
| `ugdr_device`, `ugdr_context`, `ugdr_pd`, `ugdr_cq`, `ugdr_qp` | Corresponding `ibv_*` object | aligned | Opaque public handles. F02-S02 fixes their ownership, reference, and strict child-first lifecycle behavior. |
| `ugdr_mr` | `ibv_mr` | aligned | Public fields are `context`, `pd`, `addr`, `length`, `handle`, `lkey`, and `rkey` with corresponding types and order. Keys are read directly from the returned MR. |
| `ugdr_comp_channel` | `ibv_comp_channel` | unsupported | The opaque name preserves `create_cq` signature alignment; v1 has no completion-event API. |
| `ugdr_qp_init_attr`, `ugdr_qp_attr` | `ibv_qp_init_attr`, `ibv_qp_attr` | subset adaptation | Creation capacities are flattened and unsupported fields are omitted. QP attributes expose state/current-state/access plus the standard `uint8_t` timeout/retry fields needed by the v1 connection helper; this is not the complete verbs record. |
| `ugdr_qp_attr_mask` | `ibv_qp_attr_mask` | subset adaptation | Exposed bits align exactly: state 0, current-state 1, access 3, timeout 9, retry count 10, RNR retry 11, and minimum RNR timer 15. Other mask bits are outside v1. |
| `ugdr_qp_conn_info` | No single verbs record | UGDR extension | Contains only a same-daemon `qp_num`. It is neither an address-vector record nor a serialized wire format. |
| `ugdr_sge`, `ugdr_recv_wr` | Corresponding `ibv_*` record | aligned | SGE and Receive WR match their complete standard shape. |
| `ugdr_send_wr` | `ibv_send_wr` | subset adaptation | The standard relevant prefix, anonymous `imm_data`, and `wr.rdma` nesting are preserved. Unions for unsupported opcodes and QP types are omitted. |
| `ugdr_wc` | `ibv_wc` | subset adaptation | The standard relevant base shape and anonymous `imm_data` access are preserved. The unsupported invalidated-rkey union member is omitted; v1-inapplicable success fields are zero. |
| `ugdr_qp_type` | `ibv_qp_type` | aligned | Only RC value 2 is exposed. |
| `ugdr_qp_state` | `ibv_qp_state` | aligned | Values 0 through 7 match `IBV_QPS_*`. v1 supports RESET to INIT, staged INIT to RTR to RTS, and entry to ERR; SQD/SQE transitions are unsupported. |
| `ugdr_wr_opcode` | `ibv_wr_opcode` | aligned | Only values 0 and 1 are exposed. |
| `ugdr_send_flags` | `ibv_send_flags` | aligned | Only `SIGNALED` value `1 << 1` is exposed. |
| `ugdr_wc_status` | `ibv_wc_status` | aligned | Relevant v1 names use standard numeric values and F02-S04 fixes their generation rules. |
| `ugdr_wc_opcode` | `ibv_wc_opcode` | aligned | RDMA Write is 1 and receive-with-immediate is 129. |
| `ugdr_wc_flags` | `ibv_wc_flags` | aligned | The exposed `UGDR_WC_WITH_IMM` value is `1 << 1`; other flags are outside v1. |
| `ugdr_access_flags` | `ibv_access_flags` | aligned | Local Write and Remote Write use values 1 and 2. |

## Function alignment

| UGDR public item | libibverbs item | Status | Notes |
|-|-|-|-|
| `ugdr_get_device_list` | `ibv_get_device_list` | aligned | Null plus `errno` on failure; no fake empty list. |
| `ugdr_free_device_list` | `ibv_free_device_list` | aligned | Void and no runtime action; F02-S01 additionally sets `errno` to make the unsupported placeholder observable. |
| `ugdr_open_device` | `ibv_open_device` | aligned | Null plus `errno` on failure. |
| `ugdr_close_device` | `ibv_close_device` | UGDR strict guarantee | Returns `-1` and sets `errno` on failure. UGDR deterministically reports `EBUSY` while PD or CQ children exist. |
| `ugdr_alloc_pd`, `ugdr_dealloc_pd` | `ibv_alloc_pd`, `ibv_dealloc_pd` | aligned | Pointer failure uses `errno`; deallocate returns the errno value and reports `EBUSY` while MR or QP children exist. |
| `ugdr_reg_mr` | `ibv_reg_mr` | subset adaptation | Success returns a public MR containing direct `lkey` and `rkey` fields; pointer failure uses `errno`. v1 restricts backing memory to a valid interval inside a `cudaMalloc` device allocation and transports an opaque CUDA IPC handle to the daemon. |
| `ugdr_dereg_mr` | `ibv_dereg_mr` | UGDR strict guarantee | Deregistration invalidates the handle. UGDR deterministically returns `EBUSY` while an accepted incomplete WR references the MR. |
| `ugdr_create_cq` | `ibv_create_cq` | aligned | The five-argument shape is preserved; v1 callers use a null event channel and completion vector 0. |
| `ugdr_destroy_cq` | `ibv_destroy_cq` | aligned | Returns the errno value on failure and reports `EBUSY` while any QP references the CQ. |
| `ugdr_poll_cq` | `ibv_poll_cq` | aligned | Returns up to the requested number of oldest WCs, returns 0 when empty, uses the standard negative error domain, and does not modify output on failure. |
| `ugdr_create_qp`, `ugdr_destroy_qp` | `ibv_create_qp`, `ibv_destroy_qp` | subset adaptation | Implemented RC-only creation uses a flattened init record. A QP owns SQ/RQ metadata, references each distinct CQ once, and shares one Context with its PD and CQs. Destroy removes those relationships and creates no completion. |
| `ugdr_modify_qp`, `ugdr_query_qp` | `ibv_modify_qp`, `ibv_query_qp` | subset adaptation | Uses the standard direct errno return domain and aligned exposed mask bits, but only the reviewed state/access/retry subset is public. Invalid requests fail without changing state or outputs. |
| `ugdr_query_qp_conn_info`, `ugdr_connect_qp` | Application exchange plus `ibv_modify_qp` transitions | UGDR extension | Query returns `qp_num`. Connect takes a const attribute record and requires timeout/retry/RNR/minimum-RNR masks before atomically staging INIT to RTR to RTS; it never advances the remote QP. |
| `ugdr_post_send`, `ugdr_post_recv` | `ibv_post_send`, `ibv_post_recv` | aligned | Implemented for the supported WR subset. Return domain, linked-list prefix acceptance, `bad_wr`, SQ/RQ ordering, descriptor lifetime, and capacity failure behavior follow verbs; execution-time key/range checks are deferred to the worker. |

## Lifecycle alignment and strict guarantees

| Behavior | Status | Contract |
|-|-|-|
| Context owns PD and CQ; PD owns or constrains MR and QP | aligned | Independent children must be released before their parent. |
| QP owns internal SQ/RQ and references `send_cq`/`recv_cq` | aligned | SQ/RQ have no independent v1 public handle. A CQ may serve both QP directions. |
| Same-Context QP creation | aligned | The PD, send CQ, and receive CQ must belong to one Context; mismatch fails with `EINVAL` and no partial object. |
| No cascade destruction | aligned | A public destroy operation affects only its target object. |
| Deterministic busy-resource failure | UGDR strict guarantee | Context, PD, and CQ dependency violations and MR deregistration with incomplete WR references report `EBUSY`, preserve state, and are retryable after blockers are released. |
| Deterministic invalid or stale handle failure | UGDR strict guarantee | Null, wrong-type, stale, and repeatedly destroyed handles report `EINVAL` in the function's established return domain. |
| SRQ lifecycle | unsupported | v1 has only QP-owned RQ; there is no public SRQ object or posting API. |

The strict guarantees are recorded in [Decision 0002](../decisions/0002-strict-object-lifecycle.md).

## WR, WC, and completion alignment

| Behavior | Status | Contract |
|-|-|-|
| RDMA Write | aligned | Does not consume RQ and produces only a signaled requester RDMA Write WC after remote visibility. |
| RDMA Write With Immediate | aligned | Consumes one Receive WR, produces an unconditional responder receive-with-immediate WC, and applies the normal requester signaling rule. |
| Zero-SGE Receive WR | aligned | Valid notification WR; nonzero Receive SGEs are not payload destinations for Write With Immediate. |
| RNR retry and value 7 | aligned | Uses standard `rnr_retry`/`min_rnr_timer`; value 7 means infinite retry. Exhaustion creates an error WC even for unsignaled work. |
| Error and ERR flush WCs | aligned | Error WCs ignore signaling; every incomplete SQ/RQ WR receives one flush WC on ERR, including unsignaled Send WRs. |
| Shared-CQ ordering | aligned | Per-QP/per-direction order and causality are preserved; no extra cross-source global order is promised. |
| MR busy deregistration | UGDR strict guarantee | Accepted incomplete WR references cause deterministic `EBUSY`. |

## QP state and connection alignment

| Behavior | Status | Contract |
|-|-|-|
| QP creation starts in RESET | aligned | A successful runtime create produces a live RC QP in `UGDR_QPS_RESET`. |
| RESET to INIT | aligned | `ugdr_modify_qp` requires state and access masks, exact Remote Write access, and an optional matching current-state guard. |
| INIT to RTR to RTS | UGDR extension | `ugdr_connect_qp` applies the standard state sequence as one atomic same-daemon commit using a daemon-lifetime-unique live QPN. |
| Enter ERR | aligned | RESET, INIT, RTR, and RTS can enter ERR; every incomplete SQ/RQ WR receives a flush WC. |
| Failure atomicity | UGDR strict guarantee | Invalid transitions, stale endpoints, and peer conflicts do not expose partial state, peer binding, output writes, or remote changes. |
| SQD/SQE state values | unsupported | Numeric values are retained for alignment, but transition requests return `EOPNOTSUPP`. |
| Hardware/network connection attributes | unsupported | v1 exposes no GID, LID, MTU, PSN, address vector, IP, port, or serialized connection format. Standard timeout/retry/RNR fields are exposed only through the same-daemon extension. |

The same-daemon atomic helper is recorded in
[Decision 0003](../decisions/0003-atomic-rc-connect-helper.md), and its complete state/error matrix
is in [RC QP State Machine](rc-qp-state-machine.md).

## Unsupported v1 surface

`query_device`, non-RC transports, RDMA Read, Send/Recv data operations, atomics, SRQ, completion
events, SQD/SQE transitions, hardware/network path attributes, and extended verbs are not exposed
by the reviewed F02 subset. Adding any of them requires reviewed F02 design and an updated matrix
rather than an undocumented public declaration.

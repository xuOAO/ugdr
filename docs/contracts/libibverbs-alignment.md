# libibverbs Alignment

Sources:

- [reviewed F02-S01 revision 16](../v1_docs/F02_API_契约与对象模型/F02-S01_v1_公开_API_表面与对齐基线_步骤文档.md)
- [reviewed F02-S02 revision 17](../v1_docs/F02_API_契约与对象模型/F02-S02_对象模型与生命周期契约_步骤文档.md)

Status meanings:

- **aligned**: same role, return domain, and relevant numeric constants as libibverbs.
- **UGDR extension**: UGDR-specific Client helper with no single verbs equivalent.
- **UGDR strict guarantee**: libibverbs-aligned relationship with a stronger deterministic UGDR
  failure or atomicity guarantee.
- **unsupported**: intentionally outside the v1 subset.
- **pending**: a later reviewed F02 step owns the precise fields or observable semantics.

## Type alignment

| UGDR public item | libibverbs item | Status | Notes |
|-|-|-|-|
| `ugdr_device`, `ugdr_context`, `ugdr_pd`, `ugdr_mr`, `ugdr_cq`, `ugdr_qp` | Corresponding `ibv_*` object | aligned | Opaque public handles. F02-S02 fixes their ownership, reference, and strict child-first lifecycle behavior. |
| `ugdr_comp_channel` | `ibv_comp_channel` | unsupported | The opaque name preserves `create_cq` signature alignment; v1 has no completion-event API. |
| `ugdr_qp_init_attr`, `ugdr_qp_attr` | `ibv_qp_init_attr`, `ibv_qp_attr` | pending | F02-S02 fixes the same-Context PD/CQ relationship; precise public fields and state attributes remain F02-S03. |
| `ugdr_qp_conn_info` | No single verbs record | UGDR extension | Connection exchange fields and encoding are pending F02-S03. |
| `ugdr_sge`, `ugdr_send_wr`, `ugdr_recv_wr`, `ugdr_wc` | Corresponding `ibv_*` record | pending | Public names are frozen; fields and semantics are owned by F02-S04. |
| `ugdr_qp_type` | `ibv_qp_type` | aligned | Only RC value 2 is exposed. |
| `ugdr_qp_state` | `ibv_qp_state` | aligned | Values 0 through 7 match `IBV_QPS_*`; legal transitions remain pending. |
| `ugdr_wr_opcode` | `ibv_wr_opcode` | aligned | Only values 0 and 1 are exposed. |
| `ugdr_send_flags` | `ibv_send_flags` | aligned | Only `SIGNALED` value `1 << 1` is exposed. |
| `ugdr_wc_status` | `ibv_wc_status` | aligned | Relevant v1 names use standard numeric values; generation remains pending. |
| `ugdr_wc_opcode` | `ibv_wc_opcode` | aligned | RDMA Write is 1 and receive-with-immediate is 129. |
| `ugdr_access_flags` | `ibv_access_flags` | aligned | Local Write and Remote Write use values 1 and 2. |

## Function alignment

| UGDR public item | libibverbs item | Status | Notes |
|-|-|-|-|
| `ugdr_get_device_list` | `ibv_get_device_list` | aligned | Null plus `errno` on failure; no fake empty list. |
| `ugdr_free_device_list` | `ibv_free_device_list` | aligned | Void and no runtime action; F02-S01 additionally sets `errno` to make the unsupported placeholder observable. |
| `ugdr_open_device` | `ibv_open_device` | aligned | Null plus `errno` on failure. |
| `ugdr_close_device` | `ibv_close_device` | UGDR strict guarantee | Returns `-1` and sets `errno` on failure. UGDR deterministically reports `EBUSY` while PD or CQ children exist. |
| `ugdr_alloc_pd`, `ugdr_dealloc_pd` | `ibv_alloc_pd`, `ibv_dealloc_pd` | aligned | Pointer failure uses `errno`; deallocate returns the errno value and reports `EBUSY` while MR or QP children exist. |
| `ugdr_reg_mr`, `ugdr_dereg_mr` | `ibv_reg_mr`, `ibv_dereg_mr` | aligned | Deregistration invalidates the handle; in-flight WR interaction remains F02-S04. |
| `ugdr_create_cq` | `ibv_create_cq` | aligned | The five-argument shape is preserved; v1 callers use a null event channel and completion vector 0. |
| `ugdr_destroy_cq` | `ibv_destroy_cq` | aligned | Returns the errno value on failure and reports `EBUSY` while any QP references the CQ. |
| `ugdr_poll_cq` | `ibv_poll_cq` | aligned | Uses the standard negative error domain and never writes `wc` on the placeholder path. |
| `ugdr_create_qp`, `ugdr_destroy_qp` | `ibv_create_qp`, `ibv_destroy_qp` | aligned | RC-only. A QP owns internal SQ/RQ, references its CQs, and must share one Context with its PD and CQs. Precise init fields remain F02-S03. |
| `ugdr_modify_qp`, `ugdr_query_qp` | `ibv_modify_qp`, `ibv_query_qp` | pending | Symbol shape and return domain are frozen; attr masks and fields are pending F02-S02/S03. |
| `ugdr_query_qp_conn_info`, `ugdr_connect_qp` | Application exchange plus `ibv_modify_qp` | UGDR extension | Exact connection record and transition sequence are pending F02-S03. |
| `ugdr_post_send`, `ugdr_post_recv` | `ibv_post_send`, `ibv_post_recv` | pending | Return the errno value; WR fields, `bad_wr`, and consumption semantics are pending F02-S04. |

## Lifecycle alignment and strict guarantees

| Behavior | Status | Contract |
|-|-|-|
| Context owns PD and CQ; PD owns or constrains MR and QP | aligned | Independent children must be released before their parent. |
| QP owns internal SQ/RQ and references `send_cq`/`recv_cq` | aligned | SQ/RQ have no independent v1 public handle. A CQ may serve both QP directions. |
| Same-Context QP creation | aligned | The PD, send CQ, and receive CQ must belong to one Context; mismatch fails with `EINVAL` and no partial object. |
| No cascade destruction | aligned | A public destroy operation affects only its target object. |
| Deterministic busy-parent failure | UGDR strict guarantee | Context, PD, and CQ dependency violations report `EBUSY`, preserve all state, and are retryable after blockers are released. |
| Deterministic invalid or stale handle failure | UGDR strict guarantee | Null, wrong-type, stale, and repeatedly destroyed handles report `EINVAL` in the function's established return domain. |
| SRQ lifecycle | unsupported | v1 has only QP-owned RQ; there is no public SRQ object or posting API. |

The strict guarantees are recorded in
[Decision 0002](../decisions/0002-strict-object-lifecycle.md). In-flight WR/WC destruction
behavior is not classified here until F02-S04 is reviewed.

## Unsupported v1 surface

`query_device`, non-RC transports, RDMA Read, Send/Recv data operations, atomics, SRQ, completion
events, and extended verbs are not exposed by F02-S01. Adding any of them requires reviewed F02
design and an updated matrix rather than an undocumented public declaration.

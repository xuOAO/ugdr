# libibverbs Alignment

Source: [reviewed F02-S01 revision 16](../v1_docs/F02_API_契约与对象模型/F02-S01_v1_公开_API_表面与对齐基线_步骤文档.md).

Status meanings:

- **aligned**: same role, return domain, and relevant numeric constants as libibverbs.
- **UGDR extension**: UGDR-specific Client helper with no single verbs equivalent.
- **unsupported**: intentionally outside the v1 subset.
- **pending**: a later reviewed F02 step owns the precise fields or observable semantics.

## Type alignment

| UGDR public item | libibverbs item | Status | Notes |
|-|-|-|-|
| `ugdr_device`, `ugdr_context`, `ugdr_pd`, `ugdr_mr`, `ugdr_cq`, `ugdr_qp` | Corresponding `ibv_*` object | aligned | Opaque in F02-S01; lifecycle is pending F02-S02. |
| `ugdr_comp_channel` | `ibv_comp_channel` | unsupported | The opaque name preserves `create_cq` signature alignment; v1 has no completion-event API. |
| `ugdr_qp_init_attr`, `ugdr_qp_attr` | `ibv_qp_init_attr`, `ibv_qp_attr` | pending | Public names are frozen; fields are owned by F02-S02/S03. |
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
| `ugdr_close_device` | `ibv_close_device` | aligned | Returns `-1` and sets `errno` on failure. |
| `ugdr_alloc_pd`, `ugdr_dealloc_pd` | `ibv_alloc_pd`, `ibv_dealloc_pd` | aligned | Pointer failure uses `errno`; deallocate returns the errno value. |
| `ugdr_reg_mr`, `ugdr_dereg_mr` | `ibv_reg_mr`, `ibv_dereg_mr` | aligned | F02-S01 does not expose MR fields yet. |
| `ugdr_create_cq` | `ibv_create_cq` | aligned | The five-argument shape is preserved; v1 callers use a null event channel and completion vector 0. |
| `ugdr_destroy_cq` | `ibv_destroy_cq` | aligned | Returns the errno value on failure. |
| `ugdr_poll_cq` | `ibv_poll_cq` | aligned | Uses the standard negative error domain and never writes `wc` on the placeholder path. |
| `ugdr_create_qp`, `ugdr_destroy_qp` | `ibv_create_qp`, `ibv_destroy_qp` | aligned | RC-only; init record fields remain pending. |
| `ugdr_modify_qp`, `ugdr_query_qp` | `ibv_modify_qp`, `ibv_query_qp` | pending | Symbol shape and return domain are frozen; attr masks and fields are pending F02-S02/S03. |
| `ugdr_query_qp_conn_info`, `ugdr_connect_qp` | Application exchange plus `ibv_modify_qp` | UGDR extension | Exact connection record and transition sequence are pending F02-S03. |
| `ugdr_post_send`, `ugdr_post_recv` | `ibv_post_send`, `ibv_post_recv` | pending | Return the errno value; WR fields, `bad_wr`, and consumption semantics are pending F02-S04. |

## Unsupported v1 surface

`query_device`, non-RC transports, RDMA Read, Send/Recv data operations, atomics, SRQ, completion
events, and extended verbs are not exposed by F02-S01. Adding any of them requires reviewed F02
design and an updated matrix rather than an undocumented public declaration.

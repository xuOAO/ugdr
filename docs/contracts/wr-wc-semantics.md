# WR/WC and Completion Semantics

Sources:

- [reviewed F02-S04 revision 20](../v1_docs/F02_API_契约与对象模型/F02-S04_WR_WC_与完成语义契约_步骤文档.md)
- [reviewed F04-S02 revision 9](../v1_docs/F04_SQ、RQ、CQ_队列系统/F04-S02_SQ、RQ_posting_快路径_步骤文档.md)
- [reviewed F04-S04 revision 16](../v1_docs/F04_SQ、RQ、CQ_队列系统/F04-S04_Mock_Worker_与_completion_步骤文档.md)
- [reviewed F04-S05 revision 12](../v1_docs/F04_SQ、RQ、CQ_队列系统/F04-S05_公开_API_集成与性能观测_步骤文档.md)

This contract fixes the Client-visible v1 MR key, SGE, Send/Receive WR, WC, posting, signaling,
ordering, RNR, error, flush, polling, and destruction semantics for RC RDMA Write and RDMA Write
With Immediate. F04-S02 implements descriptor posting only; it does not execute work or move data.

## Public records

The declarations in `include/ugdr/api.hpp` preserve the following libibverbs field types, order,
and access paths:

| Record | Fields | Alignment boundary |
|-|-|-|
| `ugdr_mr` | `context`, `pd`, `addr`, `length`, `handle`, `lkey`, `rkey` | Matches the public `ibv_mr` layout using UGDR object types. A successful future `ugdr_reg_mr` returns a record whose keys are read directly as `mr->lkey` and `mr->rkey`; there is no UGDR key getter. |
| `ugdr_sge` | `uint64_t addr`, `uint32_t length`, `uint32_t lkey` | Matches `ibv_sge`. |
| `ugdr_send_wr` | `wr_id`, `next`, `sg_list`, `num_sge`, `opcode`, `send_flags`, anonymous `imm_data`, `wr.rdma.remote_addr`, `wr.rdma.rkey` | Preserves the relevant `ibv_send_wr` prefix and `wr.wr.rdma` access path. Unsupported opcode-specific unions are omitted, so this is a subset adaptation rather than the complete verbs record. `imm_data` is a 32-bit value in network byte order. |
| `ugdr_recv_wr` | `wr_id`, `next`, `sg_list`, `num_sge` | Matches `ibv_recv_wr`. |
| `ugdr_wc` | `wr_id`, `status`, `opcode`, `vendor_err`, `byte_len`, anonymous `imm_data`, `qp_num`, `src_qp`, `wc_flags`, `pkey_index`, `slid`, `sl`, `dlid_path_bits` | Preserves the standard relevant base `ibv_wc` shape. The invalidated-rkey union member is omitted because its operation and WC flag are unsupported, so the public record is a subset adaptation. Fields not applicable to the v1 operation are zero. |

`UGDR_WC_WITH_IMM` is `1U << 1U`, matching `IBV_WC_WITH_IMM`. On a non-success WC, only
`wr_id`, `status`, `qp_num`, and `vendor_err` are valid.

## Posting and `bad_wr`

Applications post Send WRs to a QP's SQ with `ugdr_post_send` and Receive WRs to its RQ with
`ugdr_post_recv`. Both calls return a positive errno value on failure, matching the verbs return
domain.

| Condition | Return and `bad_wr` | Queue effect |
|-|-|-|
| Entire linked list accepted | Return 0. The value of `bad_wr` is undefined; callers must not require it to be cleared. | Every WR is accepted in linked-list order. |
| First immediately detectable invalid WR | Return `EINVAL`; `*bad_wr` points to that WR. | The successful prefix remains accepted. The failing WR and its successors are not accepted. |
| SQ or RQ cannot accept the current WR | Return `ENOMEM`; `*bad_wr` points to that WR. | The successful prefix remains accepted; there is no rollback. |

Immediate validation includes null required pointers, unknown opcode or flag bits, negative
`num_sge`, a positive SGE count with a null list, SGE counts above QP capability, and a QP state
that does not permit posting. Send posting requires RTS. Receive posting is valid in INIT, RTR, and
RTS; a zero-SGE Receive WR is valid. lkey, rkey, address, range, and access checks are deferred to
worker execution and are not posting failures.

One per-QP posting mutex serializes the complete linked-list operation so the underlying queue
retains its SPSC producer contract. Concurrent posting to different QPs is independent. Accepted
descriptors contain copied scalar fields and copied SGEs, never Client WR/SGE pointers. A normal
Write stores immediate data as zero; Write With Immediate preserves the supplied network-order
value.

WR records and SGE arrays need remain valid only until the post call returns because the
implementation must copy accepted descriptors. A non-inline data buffer remains valid until its WR
completes. An unsignaled WR has no successful send WC; a later successful signaled WR on the same SQ
proves that earlier WRs have completed.

## Write and Write With Immediate

| Operation | Remote RQ | Requester completion | Responder completion |
|-|-|-|-|
| `UGDR_WR_RDMA_WRITE` | Does not inspect or consume a Receive WR. | Produces `UGDR_WC_RDMA_WRITE` only when `sq_sig_all` is 1 or the WR has `UGDR_SEND_SIGNALED`, and only after remote data is visible. | Produces no WC. |
| `UGDR_WR_RDMA_WRITE_WITH_IMM` | Consumes one Receive WR in FIFO order. The Receive WR supplies notification `wr_id`, not the RDMA Write payload destination. | Uses the same signaling rule as ordinary Write. | Always produces `UGDR_WC_RECV_RDMA_WITH_IMM` after remote data is visible, sets `UGDR_WC_WITH_IMM`, returns network-order `imm_data`, and reports total Write length in `byte_len`. |

A zero-SGE Receive WR (`num_sge == 0` and `sg_list == NULL`) is a valid v1 notification WR. A
nonzero-SGE Receive WR is accepted subject to the QP SGE limit, but Write With Immediate does not
copy payload into its SGEs and leaves those buffers unchanged.

Successful requester and responder completions each imply that the remote target data is visible.
Independent polling on the two endpoints does not establish an additional global order.

## RNR, execution errors, and ERR flush

If Write With Immediate arrives without a Receive WR, the responder returns RNR. The requester
retries according to `rnr_retry` and the responder's `min_rnr_timer`; `rnr_retry == 7` means
unbounded retry. Exhausting a finite RNR budget produces `UGDR_WC_RNR_RETRY_EXC_ERR` even for an
unsignaled WR and moves the requester QP to ERR. The failure does not consume a remote Receive WR or
produce a receive WC. RDMA Write is not atomic, so failed or retried target data is not generally
guaranteed unchanged.

The `ugdr_connect_qp` extension requires all four standard retry attributes:

| Field | Type | Required mask |
|-|-|-|
| `timeout` | `uint8_t` | `UGDR_QP_TIMEOUT = 1U << 9U` |
| `retry_cnt` | `uint8_t` | `UGDR_QP_RETRY_CNT = 1U << 10U` |
| `rnr_retry` | `uint8_t` | `UGDR_QP_RNR_RETRY = 1U << 11U` |
| `min_rnr_timer` | `uint8_t` | `UGDR_QP_MIN_RNR_TIMER = 1U << 15U` |

| Execution condition | WC status | Result |
|-|-|-|
| Invalid local length or total SGE length | `UGDR_WC_LOC_LEN_ERR` | Requester enters ERR. |
| Invalid, stale, cross-PD, or out-of-range lkey/SGE | `UGDR_WC_LOC_PROT_ERR` | Requester enters ERR. |
| Invalid, stale, cross-PD, unauthorized, or out-of-range rkey/target | `UGDR_WC_REM_ACCESS_ERR` | Requester enters ERR; responder QP state is unchanged. |
| Peer unreachable or RC retry budget exhausted | `UGDR_WC_RETRY_EXC_ERR` | Requester enters ERR. |
| RNR retry budget exhausted | `UGDR_WC_RNR_RETRY_EXC_ERR` | Requester enters ERR; no Receive WR or receive WC is consumed or produced. |
| Any incomplete SQ or RQ WR when a QP enters ERR | `UGDR_WC_WR_FLUSH_ERR` | One flush WC per incomplete WR, including unsignaled Send WRs. Existing unpolled WCs remain. |

Error WCs are never suppressed by signaling. A multi-SGE RDMA Write is not promised to update the
remote target atomically; an execution error may leave a partial update except where a pre-transfer
failure explicitly prevents the operation.

## Ordering, polling, and lifetime

- One QP executes SQ WRs in posting order and consumes RQ WRs in posting order.
- WCs from one QP and one direction preserve WR order. A shared CQ does not impose global order
  between different QPs or between send and receive sources beyond each source's causal order.
- `ugdr_poll_cq` removes and returns at most `num_entries` oldest WCs. Success is nonnegative;
  failure is a negative errno value and writes no output entries.
- Entering ERR creates required flush WCs. `ugdr_destroy_qp` creates no additional completion.
  After destruction returns, unexecuted WRs no longer access their buffers; WCs already in a CQ
  remain pollable.
- `ugdr_dereg_mr` returns `EBUSY` while an accepted incomplete WR references the MR. This
  deterministic protection is a UGDR strict guarantee stronger than libibverbs.

## Current implementation boundary

`ugdr_post_send` and `ugdr_post_recv` implement the posting rules above. F04-S03 implements owner
CQE transport and `ugdr_poll_cq`: an empty CQ returns 0, a successful poll removes the oldest
available WCs, and failures do not modify output. F04-S04 adds a test-only, explicitly progressed
Mock Worker fixture that checks descriptor consumption, signaling, Write With Immediate receive
notification, CQ backpressure, ERR flush, teardown, and lifecycle-callback ordering. It is not
linked into the daemon or any production target and does not access payload memory.

F04-S05 extracts that fixture into the test-only `ugdr_test_support` target and verifies the
complete public `ugdr_post_send`/`ugdr_post_recv` → explicit Mock progress → `ugdr_poll_cq`
metadata path across independent Client and daemon test processes. The deterministic integration
test covers same- and different-CQ routing, RQ/CQ backpressure recovery, concurrent post/poll,
public error domains, ERR flush, setup/teardown-only control requests, and warmed allocation
counts. Separate non-CTest Release benchmarks observe metadata throughput and latency without
turning environment-dependent results into an acceptance threshold.

The daemon still does not consume posted descriptors or execute RDMA operations. Real MR reference
protection and `ugdr_dereg_mr` `EBUSY` behavior, execution-error handling, RNR/retry, payload
movement, and successful remote-visibility guarantees remain unimplemented; Mock completion must
not be interpreted as a real data operation.

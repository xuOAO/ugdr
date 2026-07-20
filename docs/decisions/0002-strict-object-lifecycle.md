# Strict Object Lifecycle

## Status

Accepted on 2026-07-20 by the reviewed
[F02-S02 lifecycle design](../v1_docs/F02_API_契约与对象模型/F02-S02_对象模型与生命周期契约_步骤文档.md),
Feishu revision 17.

## Context

UGDR presents daemon-backed opaque handles through a libibverbs-aligned Client API. Parent and
referenced resources must not disappear while dependent objects remain, and stale daemon handles
must produce deterministic Client-visible failures rather than partial cleanup or accidental reuse.
The standard verbs relationships are the baseline, while the daemon boundary makes stronger,
testable failure guarantees useful.

## Decision

Adopt a strict child-first, no-cascade lifecycle for the v1 public object subset:

- Context owns PD and CQ public children. Closing a Context with either child returns `-1`, sets
  `errno=EBUSY`, and changes no state.
- PD owns or constrains MR and QP. Deallocating a PD with either child returns `EBUSY` and changes
  no state.
- QP owns its internal SQ/RQ and references `send_cq` and `recv_cq`. The QP, PD, and both CQs must
  belong to one Context. Destroying the QP destroys SQ/RQ and removes its PD/CQ relationships.
- CQ remains independently owned by its Context. Destroying a CQ referenced by any QP returns
  `EBUSY` and changes no state.
- Null, wrong-type, stale, and repeatedly destroyed handles deterministically report `EINVAL` in
  the corresponding function's established return domain.
- Every failed create or destroy operation is atomic from the Client's perspective: it creates no
  partial object, removes no relationship, and leaves a valid target retryable.

The internal handle registry, generation tracking, reference counts, IPC representation, and memory
reclamation strategy are deliberately not part of this decision.

## Consequences

- Callers must destroy resources in dependency order; UGDR never performs hidden recursive cleanup.
- Daemon implementations must retain enough type, generation, and relationship information to
  reject invalid handles and busy parents without side effects.
- CQ and PD lifetime tests can assert deterministic `EBUSY`; stale-handle tests can assert
  deterministic `EINVAL`.
- SQ/RQ are not added as public handles. Their capacity and WR/WC behavior remain owned by
  F02-S03/F02-S04.
- Destruction behavior involving in-flight WRs, flushed work, or unread completions remains
  undecided until F02-S04 and must not be inferred from this record.

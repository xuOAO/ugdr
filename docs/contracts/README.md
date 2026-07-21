# Client Contracts

This directory contains reviewed, Client-visible UGDR contracts. Feishu remains the review
source; these files are implementation-facing specifications derived only from reviewed design
snapshots.

Sources for the F02-S01 baseline:

- [F02 API 契约与对象模型功能文档](../v1_docs/F02_API_契约与对象模型/F02_API_契约与对象模型_功能文档.md), Feishu revision 45.
- [F02-S01 v1 公开 API 表面与对齐基线](../v1_docs/F02_API_契约与对象模型/F02-S01_v1_公开_API_表面与对齐基线_步骤文档.md), Feishu revision 16.

Lifecycle source:

- [F02-S02 对象模型与生命周期契约](../v1_docs/F02_API_契约与对象模型/F02-S02_对象模型与生命周期契约_步骤文档.md), Feishu revision 17.

RC QP source:

- [F02-S03 RC QP 建连与状态机契约](../v1_docs/F02_API_契约与对象模型/F02-S03_RC_QP_建连与状态机契约_步骤文档.md), Feishu revision 7.

WR/WC source:

- [F02-S04 WR/WC 与完成语义契约](../v1_docs/F02_API_契约与对象模型/F02-S04_WR_WC_与完成语义契约_步骤文档.md), Feishu revision 20.

Current contracts:

- [Public API](public-api.md): v1 symbol surface and placeholder failure rules.
- [libibverbs alignment](libibverbs-alignment.md): mapping, support status, and intentional
  differences.
- [Object lifecycle](object-lifecycle.md): ownership, references, strict child-first destruction,
  and handle failure semantics.
- [RC QP state machine](rc-qp-state-machine.md): public QP records, connection identity, legal
  transitions, error results, and atomic same-daemon connect.
- [WR/WC and completion semantics](wr-wc-semantics.md): MR keys, public WR/WC records, posting,
  Write/Immediate behavior, retry, completion, ordering, flush, and lifetime rules.

No file in this directory may define IPC encoding, internal WQE/CQE layout, worker scheduling, or
GPU metadata.

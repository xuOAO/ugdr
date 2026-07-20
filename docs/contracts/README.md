# Client Contracts

This directory contains reviewed, Client-visible UGDR contracts. Feishu remains the review
source; these files are implementation-facing specifications derived only from reviewed design
snapshots.

Sources for the F02-S01 baseline:

- [F02 API 契约与对象模型功能文档](../v1_docs/F02_API_契约与对象模型/F02_API_契约与对象模型_功能文档.md), Feishu revision 45.
- [F02-S01 v1 公开 API 表面与对齐基线](../v1_docs/F02_API_契约与对象模型/F02-S01_v1_公开_API_表面与对齐基线_步骤文档.md), Feishu revision 16.

Current contracts:

- [Public API](public-api.md): v1 symbol surface and placeholder failure rules.
- [libibverbs alignment](libibverbs-alignment.md): mapping, support status, and intentional
  differences.

Future reviewed F02 steps add object lifecycle, RC QP state-machine, and WR/WC semantics here.
No file in this directory may define IPC encoding, internal WQE/CQE layout, worker scheduling, or
GPU metadata.

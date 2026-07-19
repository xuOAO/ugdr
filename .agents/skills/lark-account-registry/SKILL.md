---
name: lark-account-registry
description: 登记、校验、列出并选择仓库 `.lark` 中声明的飞书账户，将逻辑账户解析为固定的 `lark-cli --profile ... --as ...` 命令前缀。仓库存在 `.lark/registry.json` 时，任何 lark-cli 或 lark-* Skill 操作前都必须使用；也用于用户要求登记、切换、查看或验证飞书账户。多个可用账户时返回候选并要求用户选择，不自动回退账户。
---

# 飞书账户登记与选择

使用 `scripts/account_registry.py` 管理仓库的逻辑账户和本机 `lark-cli` profile 映射。只保存 profile 名称与身份类型；密钥、token、cookie 和密码始终由 `lark-cli` 自己管理，不得写入 `.lark`。

## 飞书操作前解析账户

1. 从仓库根目录运行：

   ```bash
   python3 .agents/skills/lark-account-registry/scripts/account_registry.py \
     --root . resolve --purpose docs --purpose drive
   ```

2. 用户明确指定账户时追加 `--account-id <账户标识>`。
3. 处理结构化结果：
   - `status=resolved`：将 `argv_prefix` 视为本轮唯一 `<LARK>` 前缀。
   - `status=selection_required`：向用户展示候选的标识、标签、身份和用途；得到选择前停止飞书操作，然后带 `--account-id` 重新解析。
   - `status=error`：报告错误并停止；不得改用默认 profile 或其他账户。
4. 使用解析出的前缀运行 `<LARK> whoami`。确认身份可用后，再读取或修改飞书资源。
5. 本轮全部飞书 Skills 和命令复用同一前缀。权限、scope、token 或资源访问失败时按 `lark-shared` 处理，不自动切换账户。

`--purpose` 可以重复，候选账户必须覆盖全部用途；账户用途包含 `*` 时可参与任意飞书操作。

## 登记账户

先读取 `lark-shared` Skill，确认用户希望登记的账户标识、用途、身份和本机 profile。若 profile 尚未配置或授权，先按 `lark-shared` 完成配置或授权；授权链接、device code 和 token 不得写入 `.lark`。profile 可用后再执行：

```bash
python3 .agents/skills/lark-account-registry/scripts/account_registry.py \
  --root . register \
  --account-id ugdr-user \
  --label "UGDR 用户账户" \
  --profile cli_xxx \
  --as user \
  --purpose docs \
  --purpose drive
```

登记前脚本会用指定 profile 执行 `whoami`。账户已存在时默认拒绝覆盖；只有用户明确要求更新该账户时才使用 `--replace`。

登记结果分为：

- `.lark/accounts/<account-id>.json`：可提交的逻辑账户描述，不含本机凭证。
- `.lark/local/profiles.json`：逻辑账户到本机 profile 的映射，由 `.lark/.gitignore` 排除。

## 查看与验证

```bash
python3 .agents/skills/lark-account-registry/scripts/account_registry.py --root . list
python3 .agents/skills/lark-account-registry/scripts/account_registry.py --root . validate
python3 .agents/skills/lark-account-registry/scripts/account_registry.py \
  --root . validate --account-id ugdr-user
```

`validate` 只验证登记结构、profile 和实际身份；具体业务 scope 仍由目标飞书操作验证。

## 安全规则

- 禁止在 `.lark` 的任何 JSON 中出现 secret、access token、refresh token、password、cookie 或 credential 字段。
- 不把 `whoami` 结果中的个人信息复制到可提交账户描述；只记录用户确认的标签、身份和用途。
- 多账户时不依据最近使用、文件 owner、当前默认 profile 或猜测自动选择。
- 用户选择只在当前任务内生效，不写入持久化“当前账户”。

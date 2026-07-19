# Repository Agent Instructions

## Feishu account routing

- Before any `lark-cli` command or `lark-*` Skill operation, read `.agents/skills/lark-account-registry/SKILL.md` and resolve an account from `.lark`.
- When multiple accounts are eligible and the user did not name one, ask the user to select; do not choose or persist a selection automatically.
- Reuse the resolved `argv_prefix` for every Feishu command in the current task. Do not fall back to another profile after an authentication, scope, permission, or resource error.
- Never store Feishu secrets, tokens, cookies, passwords, or credentials in `.lark`.

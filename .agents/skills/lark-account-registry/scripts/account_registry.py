#!/usr/bin/env python3

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple


ACCOUNT_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,62}$")
FORBIDDEN_KEY_PARTS = (
    "secret",
    "access_token",
    "refresh_token",
    "password",
    "cookie",
    "credential",
)


class RegistryError(Exception):
    pass


def emit(payload: Dict) -> None:
    print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))


def load_json_object(path: Path) -> Dict:
    try:
        with path.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        raise RegistryError("{}: {}".format(path, error)) from error
    if not isinstance(value, dict):
        raise RegistryError("{}: expected a JSON object".format(path))
    reject_secret_keys(value, str(path))
    return value


def reject_secret_keys(value, location: str) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = str(key).lower()
            if any(part in normalized for part in FORBIDDEN_KEY_PARTS):
                raise RegistryError("{}: forbidden credential field {}".format(location, key))
            reject_secret_keys(child, location)
    elif isinstance(value, list):
        for child in value:
            reject_secret_keys(child, location)


def atomic_write_json(path: Path, value: Dict, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".tmp-", dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(value, handle, ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
        os.chmod(temporary_name, mode)
        os.replace(temporary_name, str(path))
    except Exception:
        try:
            os.unlink(temporary_name)
        except OSError:
            pass
        raise


def resolve_inside(base: Path, relative: str, field: str) -> Path:
    candidate = Path(relative)
    if candidate.is_absolute():
        raise RegistryError("{} must be relative to .lark".format(field))
    resolved_base = base.resolve()
    resolved = (base / candidate).resolve()
    if resolved != resolved_base and resolved_base not in resolved.parents:
        raise RegistryError("{} escapes .lark: {}".format(field, relative))
    return resolved


def validate_registry(value: Dict, path: Path) -> Tuple[str, str]:
    expected = {
        "schema_version",
        "accounts_dir",
        "local_profiles_file",
        "selection_policy",
    }
    unknown = sorted(set(value) - expected)
    if unknown:
        raise RegistryError("{}: unknown fields: {}".format(path, ", ".join(unknown)))
    if value.get("schema_version") != 1:
        raise RegistryError("{}: schema_version must be 1".format(path))
    accounts_dir = value.get("accounts_dir")
    profiles_file = value.get("local_profiles_file")
    if not isinstance(accounts_dir, str) or not accounts_dir:
        raise RegistryError("{}: accounts_dir must be a non-empty string".format(path))
    if not isinstance(profiles_file, str) or not profiles_file:
        raise RegistryError("{}: local_profiles_file must be a non-empty string".format(path))
    if value.get("selection_policy") != "ask_when_multiple":
        raise RegistryError("{}: selection_policy must be ask_when_multiple".format(path))
    return accounts_dir, profiles_file


def validate_account(value: Dict, path: Path) -> Dict:
    expected = {"schema_version", "account_id", "label", "identity", "purposes", "enabled"}
    unknown = sorted(set(value) - expected)
    if unknown:
        raise RegistryError("{}: unknown fields: {}".format(path, ", ".join(unknown)))
    if value.get("schema_version") != 1:
        raise RegistryError("{}: schema_version must be 1".format(path))
    account_id = value.get("account_id")
    if not isinstance(account_id, str) or not ACCOUNT_ID_RE.fullmatch(account_id):
        raise RegistryError("{}: invalid account_id".format(path))
    if path.stem != account_id:
        raise RegistryError("{}: filename must match account_id".format(path))
    if not isinstance(value.get("label"), str) or not value["label"].strip():
        raise RegistryError("{}: label must be a non-empty string".format(path))
    if value.get("identity") not in ("user", "bot"):
        raise RegistryError("{}: identity must be user or bot".format(path))
    purposes = value.get("purposes")
    if (
        not isinstance(purposes, list)
        or not purposes
        or not all(isinstance(item, str) and item for item in purposes)
        or len(purposes) != len(set(purposes))
    ):
        raise RegistryError("{}: purposes must be a non-empty unique string array".format(path))
    if not isinstance(value.get("enabled"), bool):
        raise RegistryError("{}: enabled must be boolean".format(path))
    return value


def validate_profiles(value: Dict, path: Path) -> Dict[str, Dict[str, str]]:
    expected = {"schema_version", "profiles"}
    unknown = sorted(set(value) - expected)
    if unknown:
        raise RegistryError("{}: unknown fields: {}".format(path, ", ".join(unknown)))
    if value.get("schema_version") != 1:
        raise RegistryError("{}: schema_version must be 1".format(path))
    profiles = value.get("profiles")
    if not isinstance(profiles, dict):
        raise RegistryError("{}: profiles must be an object".format(path))
    for account_id, mapping in profiles.items():
        if not ACCOUNT_ID_RE.fullmatch(account_id):
            raise RegistryError("{}: invalid mapped account_id {}".format(path, account_id))
        if not isinstance(mapping, dict) or set(mapping) != {"profile", "as"}:
            raise RegistryError("{}: mapping {} must contain only profile and as".format(path, account_id))
        if not isinstance(mapping.get("profile"), str) or not mapping["profile"].strip():
            raise RegistryError("{}: mapping {} has invalid profile".format(path, account_id))
        if mapping.get("as") not in ("user", "bot"):
            raise RegistryError("{}: mapping {} as must be user or bot".format(path, account_id))
    return profiles


def load_state(root: Path):
    lark_dir = root.resolve() / ".lark"
    registry_path = lark_dir / "registry.json"
    if not registry_path.is_file():
        raise RegistryError("missing {}".format(registry_path))
    registry = load_json_object(registry_path)
    accounts_relative, profiles_relative = validate_registry(registry, registry_path)
    accounts_dir = resolve_inside(lark_dir, accounts_relative, "accounts_dir")
    profiles_path = resolve_inside(lark_dir, profiles_relative, "local_profiles_file")

    accounts = {}
    if not accounts_dir.is_dir():
        raise RegistryError("missing accounts directory {}".format(accounts_dir))
    for path in sorted(accounts_dir.glob("*.json")):
        account = validate_account(load_json_object(path), path)
        account_id = account["account_id"]
        if account_id in accounts:
            raise RegistryError("duplicate account_id {}".format(account_id))
        accounts[account_id] = account

    if profiles_path.exists():
        profiles_value = load_json_object(profiles_path)
    else:
        profiles_value = {"schema_version": 1, "profiles": {}}
    profiles = validate_profiles(profiles_value, profiles_path)
    unknown_mappings = sorted(set(profiles) - set(accounts))
    if unknown_mappings:
        raise RegistryError(
            "{}: mappings reference unknown accounts: {}".format(
                profiles_path, ", ".join(unknown_mappings)
            )
        )
    return lark_dir, accounts_dir, profiles_path, accounts, profiles


def purpose_matches(account: Dict, requested: List[str]) -> bool:
    purposes = set(account["purposes"])
    return "*" in purposes or set(requested).issubset(purposes)


def account_summary(account: Dict, mapping: Optional[Dict]) -> Dict:
    result = {
        "account_id": account["account_id"],
        "enabled": account["enabled"],
        "identity": account["identity"],
        "label": account["label"],
        "mapped": mapping is not None,
        "purposes": account["purposes"],
    }
    if mapping is not None:
        result["profile"] = mapping["profile"]
    return result


def validate_profile(profile: str, identity: str) -> Dict:
    command = ["lark-cli", "--profile", profile, "--as", identity, "whoami"]
    try:
        process = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
    except OSError as error:
        raise RegistryError("unable to run lark-cli: {}".format(error)) from error
    if process.returncode != 0:
        detail = process.stderr.strip() or process.stdout.strip() or "unknown error"
        raise RegistryError("whoami failed for profile {}: {}".format(profile, detail))
    try:
        value = json.loads(process.stdout)
    except json.JSONDecodeError as error:
        raise RegistryError("whoami returned invalid JSON for profile {}: {}".format(profile, error))
    if not isinstance(value, dict):
        raise RegistryError("whoami returned a non-object JSON value for profile {}".format(profile))
    if value.get("profile") != profile:
        raise RegistryError("whoami profile mismatch: expected {}, got {}".format(profile, value.get("profile")))
    if value.get("identity") != identity:
        raise RegistryError(
            "whoami identity mismatch for {}: expected {}, got {}".format(
                profile, identity, value.get("identity")
            )
        )
    if value.get("available") is not True:
        raise RegistryError("profile {} identity {} is unavailable".format(profile, identity))
    return {
        "available": True,
        "identity": identity,
        "profile": profile,
        "token_status": value.get("tokenStatus"),
    }


def command_list(args) -> int:
    _, _, _, accounts, profiles = load_state(Path(args.root))
    emit(
        {
            "status": "ok",
            "accounts": [
                account_summary(accounts[account_id], profiles.get(account_id))
                for account_id in sorted(accounts)
            ],
        }
    )
    return 0


def command_resolve(args) -> int:
    _, _, _, accounts, profiles = load_state(Path(args.root))
    requested = args.purpose or []

    if args.account_id:
        account = accounts.get(args.account_id)
        if account is None:
            raise RegistryError("unknown account_id {}".format(args.account_id))
        if not account["enabled"]:
            raise RegistryError("account {} is disabled".format(args.account_id))
        if not purpose_matches(account, requested):
            raise RegistryError(
                "account {} does not cover purposes: {}".format(
                    args.account_id, ", ".join(requested)
                )
            )
        candidate_ids = [args.account_id]
    else:
        candidate_ids = [
            account_id
            for account_id, account in sorted(accounts.items())
            if account["enabled"] and purpose_matches(account, requested)
        ]

    mapped_ids = [account_id for account_id in candidate_ids if account_id in profiles]
    if not mapped_ids:
        raise RegistryError(
            "no mapped account covers purposes: {}".format(
                ", ".join(requested) if requested else "any"
            )
        )
    if len(mapped_ids) > 1:
        emit(
            {
                "status": "selection_required",
                "requested_purposes": requested,
                "candidates": [
                    account_summary(accounts[account_id], profiles[account_id])
                    for account_id in mapped_ids
                ],
            }
        )
        return 2

    account_id = mapped_ids[0]
    account = accounts[account_id]
    mapping = profiles[account_id]
    if mapping["as"] != account["identity"]:
        raise RegistryError(
            "identity mismatch for {} between account descriptor and local mapping".format(account_id)
        )
    emit(
        {
            "status": "resolved",
            "account": account_summary(account, mapping),
            "requested_purposes": requested,
            "argv_prefix": [
                "lark-cli",
                "--profile",
                mapping["profile"],
                "--as",
                mapping["as"],
            ],
        }
    )
    return 0


def command_validate(args) -> int:
    _, _, _, accounts, profiles = load_state(Path(args.root))
    if args.account_id:
        if args.account_id not in accounts:
            raise RegistryError("unknown account_id {}".format(args.account_id))
        account_ids = [args.account_id]
    else:
        account_ids = sorted(accounts)

    results = []
    failures = []
    for account_id in account_ids:
        account = accounts[account_id]
        mapping = profiles.get(account_id)
        if mapping is None:
            failures.append("account {} has no local profile mapping".format(account_id))
            continue
        if mapping["as"] != account["identity"]:
            failures.append("account {} identity does not match local mapping".format(account_id))
            continue
        try:
            result = validate_profile(mapping["profile"], mapping["as"])
        except RegistryError as error:
            failures.append(str(error))
        else:
            result["account_id"] = account_id
            results.append(result)
    if failures:
        emit({"status": "error", "errors": failures, "validated": results})
        return 1
    emit({"status": "ok", "validated": results})
    return 0


def command_register(args) -> int:
    root = Path(args.root)
    _, accounts_dir, profiles_path, accounts, profiles = load_state(root)
    account_id = args.account_id
    if not ACCOUNT_ID_RE.fullmatch(account_id):
        raise RegistryError("invalid account_id {}".format(account_id))
    if account_id in accounts and not args.replace:
        raise RegistryError("account {} already exists; use --replace only after confirmation".format(account_id))
    purposes = args.purpose or []
    if not purposes or len(purposes) != len(set(purposes)):
        raise RegistryError("provide at least one unique --purpose")

    profile_result = validate_profile(args.profile, args.identity)
    account = {
        "schema_version": 1,
        "account_id": account_id,
        "label": args.label,
        "identity": args.identity,
        "purposes": purposes,
        "enabled": True,
    }
    validate_account(account, accounts_dir / "{}.json".format(account_id))
    new_profiles = dict(profiles)
    new_profiles[account_id] = {"profile": args.profile, "as": args.identity}
    profiles_value = {"schema_version": 1, "profiles": new_profiles}
    validate_profiles(profiles_value, profiles_path)

    atomic_write_json(accounts_dir / "{}.json".format(account_id), account)
    atomic_write_json(profiles_path, profiles_value, mode=0o600)
    emit(
        {
            "status": "registered",
            "account": account_summary(account, new_profiles[account_id]),
            "whoami": profile_result,
        }
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Manage repository Lark account registrations.")
    parser.add_argument("--root", default=".", help="Repository root containing .lark.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    list_parser = subparsers.add_parser("list", help="List registered accounts.")
    list_parser.set_defaults(handler=command_list)

    resolve_parser = subparsers.add_parser("resolve", help="Resolve an account for one task.")
    resolve_parser.add_argument("--account-id")
    resolve_parser.add_argument("--purpose", action="append", default=[])
    resolve_parser.set_defaults(handler=command_resolve)

    validate_parser = subparsers.add_parser("validate", help="Validate local lark-cli profiles.")
    validate_parser.add_argument("--account-id")
    validate_parser.set_defaults(handler=command_validate)

    register_parser = subparsers.add_parser("register", help="Register a logical account and profile.")
    register_parser.add_argument("--account-id", required=True)
    register_parser.add_argument("--label", required=True)
    register_parser.add_argument("--profile", required=True)
    register_parser.add_argument("--as", dest="identity", choices=("user", "bot"), required=True)
    register_parser.add_argument("--purpose", action="append", default=[])
    register_parser.add_argument("--replace", action="store_true")
    register_parser.set_defaults(handler=command_register)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.handler(args)
    except RegistryError as error:
        emit({"status": "error", "message": str(error)})
        return 1


if __name__ == "__main__":
    sys.exit(main())

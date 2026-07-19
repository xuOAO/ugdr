#!/usr/bin/env python3
"""Compute or verify the normalized body hash of a generated Markdown file."""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

import yaml


def parse_document(path: Path) -> tuple[dict[str, object], bytes]:
    raw = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    if not raw.startswith(b"---\n"):
        raise ValueError("document does not start with YAML frontmatter")

    end = raw.find(b"\n---\n", 4)
    if end < 0:
        raise ValueError("frontmatter closing delimiter was not found")

    metadata = yaml.safe_load(raw[4:end].decode("utf-8"))
    if not isinstance(metadata, dict):
        raise ValueError("frontmatter must be a YAML mapping")

    body = raw[end + 5 :].rstrip(b"\n") + b"\n"
    return metadata, body


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compute the SHA-256 of the normalized Markdown body."
    )
    parser.add_argument("path", type=Path)
    parser.add_argument(
        "--check",
        action="store_true",
        help="compare the computed hash with generated_body_sha256",
    )
    args = parser.parse_args()

    try:
        metadata, body = parse_document(args.path)
    except (OSError, UnicodeDecodeError, ValueError, yaml.YAMLError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    digest = hashlib.sha256(body).hexdigest()
    if not args.check:
        print(digest)
        return 0

    expected = metadata.get("generated_body_sha256")
    if expected == digest:
        print(f"ok: {digest}")
        return 0

    print(f"mismatch: expected={expected!r} actual={digest}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

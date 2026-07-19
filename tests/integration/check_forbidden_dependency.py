#!/usr/bin/env python3

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify that a forbidden edge is rejected.")
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--checker", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--build-dir", required=True)
    return parser.parse_args()


def run(command):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main() -> int:
    args = parse_args()
    fixture = Path(args.fixture).resolve()
    build_dir = Path(args.build_dir).resolve()
    if build_dir.exists():
        shutil.rmtree(str(build_dir))
    query_dir = build_dir / ".cmake" / "api" / "v1" / "query"
    query_dir.mkdir(parents=True)
    (query_dir / "codemodel-v2").touch()

    configure = run([args.cmake, "-S", str(fixture), "-B", str(build_dir), "-G", "Ninja"])
    if configure.returncode != 0:
        sys.stderr.write(configure.stdout)
        sys.stderr.write(configure.stderr)
        return 1

    check = run(
        [
            sys.executable,
            args.checker,
            "--root",
            str(fixture),
            "--build-dir",
            str(build_dir),
        ]
    )
    output = check.stdout + check.stderr
    expected = [
        "forbidden dependency: ugdr_api -> ugdr_test_support",
        "forbidden dependency: ugdr_api -> ugdr_worker",
    ]
    if check.returncode == 0:
        sys.stderr.write("checker accepted a forbidden dependency\n")
        sys.stderr.write(output)
        return 1
    missing = [diagnostic for diagnostic in expected if diagnostic not in output]
    if missing:
        sys.stderr.write(
            "checker failed without the expected diagnostics: {}\n".format(", ".join(missing))
        )
        sys.stderr.write(output)
        return 1

    print("forbidden dependencies rejected: {}".format(", ".join(expected)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

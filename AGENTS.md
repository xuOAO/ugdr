# Repository Guidelines

## Project Structure & Module Organization
- `src/libugdr/` contains the client library implementation and public-facing API wiring.
- `src/daemon/` holds the daemon entrypoint and core runtime (drivers, IPC server, and resource managers).
- `src/common/` contains shared utilities (logging, IPC helpers, CUDA helpers, types).
- `include/` exposes public headers for the library (`ugdr.h`).
- `tests/` houses GoogleTest-based unit/integration tests plus fixtures.
- `doc/` has technical documentation such as `doc/api.md`.
- `build/` is the default output directory for objects, binaries, and test executables.

## Build, Test, and Development Commands
- `make` builds libugdr, the daemon, and tests in release mode.
- `make debug` builds everything with debug flags (`-g -O0`).
- `make libugdr` builds only the shared library.
- `make daemon` builds only the daemon executable.
- `make tests` builds the test runner at `build/bin/tests/run_tests`.
- `make clean` removes `build/` artifacts.

## Coding Style & Naming Conventions
- C++20 is required (`-std=c++20`); keep code warning-clean with `-Wall -Wextra`.
- Indentation is 4 spaces; braces follow K&R style seen in existing headers.
- Classes/types use `PascalCase` (e.g., `Manager`), functions and members use `snake_case` with trailing underscores for member fields (e.g., `config_`).
- Keep headers as `.h` and sources as `.cpp`; prefer small, focused translation units.

## Testing Guidelines
- Tests use GoogleTest via `tests/*.cpp` and are linked in `tests/Makefile`.
- Name tests by feature area (e.g., `connection_test.cpp`, `ipc_test.cpp`).
- Run locally with `./build/bin/tests/run_tests` after `make tests`.

## Commit & Pull Request Guidelines
- Recent history uses short, imperative subjects like `Refactor: ...` or Conventional Commits such as `feat(scope): ...`.
- Match that style and keep subjects under ~70 characters.
- PRs should describe behavior changes, include test results, and link related issues or design docs when applicable.

## Configuration & Runtime Notes
- Daemon configuration helpers live in `src/daemon/utils/`; config files typically sit in `config/`.
- If you add new runtime flags or protocol fields, update `doc/api.md` and any relevant IPC headers in `src/common/ipc/`.

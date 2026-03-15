# CLAUDE.md

## Workflow conventions

### Wrapper scripts for noisy commands

Create small shell scripts that suppress verbose output and print a single-line result (e.g. "PASS"/"FAIL", "BUILD OK"/"BUILD FAILED"). This avoids wasting tokens on hundreds of lines of build/test output when only the outcome matters. If the command fails, run it again directly (without the wrapper) to inspect the full error output.

Existing scripts:
- `scripts/build.sh` — formats code then builds the project, prints BUILD OK/BUILD FAILED (shows full output on failure)
- `scripts/run_tests.sh` — builds and runs all unit tests, prints PASS/FAIL
- `scripts/format.sh` — runs clang-format on all src/ and tests/ files

When adding new long-running or noisy commands to the workflow, prefer creating a similar wrapper script first.

## Git

If all tests pass and your changes are complete, then run `scripts/format.sh` and go ahead and push to origin.

## Testing

After making any code changes and before pushing to git, always run the test suite:

```bash
./scripts/run_tests.sh
```

If tests fail, run `./build/trace_render_tests` directly to see detailed failure output. Do not push code that fails tests.

When modifying business logic (model, parser, indexing, view state, or any non-UI code), always add or update corresponding unit tests in `tests/`.

## Build

```bash
cmake -B build
cmake --build build
```

## Pre-computing derived data

Derived data (e.g. unique categories, self times, depth, parent indices) should be computed once in `TraceModel::build_index()`, not per-frame in UI code. If a value can be calculated at index time, store it on the model and look it up during rendering. This keeps the render loop fast and avoids redundant O(n) scans every frame.

## Time formatting

Use `format_time()` from `src/ui/format_time.h` whenever displaying time values in the UI. Do not write inline time formatting or create local `format_time` variants. The function takes microseconds and auto-selects the appropriate unit (ns/us/ms/s). For ruler tick labels, use `format_ruler_time()` from the same header.

## Encapsulation

Classes use private fields with getter/setter accessor methods. POD structs (e.g. `TraceEvent`, `ThreadInfo`, `ProcessInfo`, `CounterSeries`, `DiagStats`) may use public fields directly. As soon as a POD needs a method (validation, invariant enforcement, derived computation), convert it to the accessor pattern with private fields. Prefer domain-specific mutation methods (e.g. `add_search_result()`, `navigate_to_event()`) over exposing raw mutable references.

## Project Structure

- `src/` — application source code
- `tests/` — Google Test unit tests
- `scripts/` — wrapper scripts for noisy commands (see below)
  - `build.sh` — formats code then builds, prints BUILD OK/BUILD FAILED
  - `run_tests.sh` — builds and runs tests, prints PASS/FAIL
  - `format.sh` — runs clang-format on src/ and tests/

## Source index files

Each source directory contains an `INDEX.md` with a one-line directory description, a one-line description per file, and all public function signatures (one per line).

- `src/INDEX.md`
- `src/model/INDEX.md`
- `src/parser/INDEX.md`
- `src/platform/INDEX.md`
- `src/ui/INDEX.md`

**When modifying a source file, update the corresponding `INDEX.md`** — add, remove, or update signatures to match. Keep descriptions to one sentence.

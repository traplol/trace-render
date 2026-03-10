# CLAUDE.md

## Workflow conventions

### Wrapper scripts for noisy commands

Create small shell scripts that suppress verbose output and print a single-line result (e.g. "PASS"/"FAIL", "BUILD OK"/"BUILD FAILED"). This avoids wasting tokens on hundreds of lines of build/test output when only the outcome matters. If the command fails, run it again directly (without the wrapper) to inspect the full error output.

Existing scripts:
- `scripts/build.sh` — builds the project, prints BUILD OK/BUILD FAILED (shows full output on failure)
- `scripts/run_tests.sh` — builds and runs all unit tests, prints PASS/FAIL

When adding new long-running or noisy commands to the workflow, prefer creating a similar wrapper script first.

## Testing

After making any code changes and before pushing to git, always run the test suite:

```bash
./scripts/run_tests.sh
```

If tests fail, run `./build/perfetto_tests` directly to see detailed failure output. Do not push code that fails tests.

When modifying business logic (model, parser, indexing, view state, or any non-UI code), always add or update corresponding unit tests in `tests/`.

## Build

```bash
cmake -B build
cmake --build build
```

## Project Structure

- `src/` — application source code
- `tests/` — Google Test unit tests
- `scripts/` — wrapper scripts for noisy commands (see below)
  - `build.sh` — builds project, prints BUILD OK/BUILD FAILED
  - `run_tests.sh` — builds and runs tests, prints PASS/FAIL

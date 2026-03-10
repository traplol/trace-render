# CLAUDE.md

## Testing

After making any code changes and before pushing to git, always run the test suite:

```bash
./run_tests.sh
```

This builds and runs all unit tests, printing "PASS" or "FAIL". If tests fail, run `./build/perfetto_tests` directly to see detailed failure output. Do not push code that fails tests.

## Build

```bash
cmake -B build
cmake --build build
```

## Project Structure

- `src/` — application source code
- `tests/` — Google Test unit tests
- `run_tests.sh` — test runner script (prints PASS/FAIL)

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

find src tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i
echo "FORMAT OK"

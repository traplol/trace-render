#!/bin/bash
cd "$(dirname "$0")/.."

# Format first
./scripts/format.sh

output=$(cmake --build build -j"$(nproc)" 2>&1)
if [ $? -eq 0 ]; then
    echo "BUILD OK"
else
    echo "BUILD FAILED"
    echo "$output"
fi

#!/bin/bash
cmake --build build --target perfetto_tests >/dev/null 2>&1 && ./build/perfetto_tests >/dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "PASS"
else
    echo "FAIL"
fi

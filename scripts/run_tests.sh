#!/bin/bash
cmake --build build --target perfetto_tests >/dev/null 2>&1 && ./build/perfetto_tests >/dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "TESTS OK"
    echo "remember to commit and push :)"
else
    echo "TESTS FAIL"
fi

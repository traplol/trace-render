#!/bin/bash
cmake --build build -j"$(nproc)" --target trace_render_tests >/dev/null 2>&1 && ./build/trace_render_tests >/dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "TESTS OK"
    echo "remember to commit and push :)"
else
    echo "TESTS FAIL"
fi

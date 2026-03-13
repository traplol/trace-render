#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-wasm"

# Check for Emscripten
if ! command -v emcmake &>/dev/null; then
    echo "ERROR: Emscripten not found. Install it and source emsdk_env.sh first."
    echo "  git clone https://github.com/emscripten-core/emsdk.git"
    echo "  cd emsdk && ./emsdk install latest && ./emsdk activate latest"
    echo "  source emsdk_env.sh"
    exit 1
fi

echo "Configuring WASM build..."
emcmake cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    2>&1 | tail -1

echo "Building..."
cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | tail -1

if [ -f "$BUILD_DIR/index.html" ]; then
    echo "BUILD OK"
    echo ""
    echo "Output in: $BUILD_DIR/"
    echo "  index.html  — main page"
    echo "  index.js    — JS glue"
    echo "  index.wasm  — WASM binary"
    echo ""
    echo "To serve locally:"
    echo "  cd $BUILD_DIR && python3 -m http.server 8080"
    echo "  Then open http://localhost:8080/index.html"
else
    echo "BUILD FAILED"
    exit 1
fi

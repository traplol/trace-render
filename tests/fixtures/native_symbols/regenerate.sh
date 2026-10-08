#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
fixture_build="$(mktemp -d)"
trap 'rm -rf "$fixture_build"' EXIT
clang-18 --target=x86_64-pc-windows-msvc -g -gcodeview -O0 \
    -fdebug-compilation-dir=/trace-render-fixture \
    -c tests/fixtures/native_symbols/fixture.c -o "$fixture_build/fixture.obj"
"${LLD_LINK:-lld-link-18}" /entry:fixture_entry /subsystem:console /nodefaultlib \
    /debug:full /Brepro /opt:noref /opt:noicf /pdbaltpath:fixture.pdb \
    "/out:$fixture_build/fixture.exe" "/pdb:$fixture_build/fixture.pdb" "$fixture_build/fixture.obj"
cp "$fixture_build/fixture.exe" "$fixture_build/fixture.pdb" tests/fixtures/native_symbols/
llvm-pdbutil-18 dump -summary "$fixture_build/fixture.pdb"
echo 'Update the expected GUID in test_native_symbols.cpp after regenerating.'

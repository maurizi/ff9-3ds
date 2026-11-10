#!/usr/bin/env bash
# rabbitizer (splat's instruction decoder, a C extension) as a Pyodide wheel, into OUT_DIR (default: the current
# directory). Runs in a python:3.14 container (tools: pyodide-build and the Emscripten it installs):
#   docker run --rm -v "$PWD/builder/web:/w" -v "$PWD/build/web-wheels:/out" -w /w python:3.14-bookworm \
#       bash build_rabbitizer.sh /out
set -euo pipefail
OUT=${1:-$PWD}
PYODIDE_VERSION=$(cat "$(dirname "$0")/PYODIDE_VERSION")
pip -q install pyodide-build
pyodide xbuildenv install "$PYODIDE_VERSION" > /dev/null
pyodide xbuildenv install-emscripten > /dev/null
work=$(mktemp -d)
pip -q download --no-binary :all: --no-deps rabbitizer==1.16.2 -d "$work"
cd "$work" && tar xf rabbitizer-1.16.2.tar.gz && cd rabbitizer-1.16.2
# wasm32's long is 32 bits: the bindings return values (uint32 addresses above 2^31 among them: the register
# tracker's symbol addresses) with PyLong_FromLong, which would make them negative and lose every symbol's access
# type in the split. long long returns what a 64-bit build does.
grep -rl "PyLong_FromLong(" rabbitizer | xargs sed -i 's/PyLong_FromLong(/PyLong_FromLongLong((long long)/g'
pyodide build -o "$OUT" | tail -1

#!/usr/bin/env bash
# Host build + test of the PC tools: cmake configure (AB_TARGET=dev), ninja build, ctest. Exit code = result.
# Needs cmake, ninja and a C++14 compiler; the autobleem-core submodule must be initialised.
# Usage: ci/host-test.sh [build-dir]   (default: build-host-test next to this repo's CMakeLists.txt)
set -euo pipefail
src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$src/build-host-test}"
[ -f "$src/autobleem-core/CMakeLists.txt" ] || { echo "autobleem-core submodule missing: git submodule update --init" >&2; exit 2; }
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DAB_TARGET=dev
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure

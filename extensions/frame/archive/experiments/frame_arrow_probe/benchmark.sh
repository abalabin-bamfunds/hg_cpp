#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../../../.." && pwd)"
mkdir -p "$script_dir/build"

c++ -std=c++23 -O3 -DNDEBUG -shared -fPIC -Wall -Wextra -Wpedantic \
    -I"$repo_root/include" "$script_dir/probe.cpp" \
    -o "$script_dir/build/libframe_arrow_probe.so"

python "$script_dir/benchmark.py" \
    --library "$script_dir/build/libframe_arrow_probe.so" \
    --output "$script_dir/benchmark_results.md"

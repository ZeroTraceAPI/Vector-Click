#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
zig_bin="${VECTORCLICK_ZIG:-zig}"
expected_version="0.16.0"
actual_version=$("$zig_bin" version 2>/dev/null || true)

if [ "$actual_version" != "$expected_version" ]; then
    printf '%s\n' \
        "Vector Click Zig build requires Zig $expected_version." \
        "Set VECTORCLICK_ZIG to that Zig executable. Detected: ${actual_version:-unavailable}" >&2
    exit 2
fi

build_dir="${VECTORCLICK_ZIG_BUILD_DIR:-$source_dir/build/zig-0.16.0-release}"
cache_root="${VECTORCLICK_ZIG_CACHE_DIR:-$build_dir/zig-cache}"
export VECTORCLICK_ZIG="$zig_bin"
export ZIG_GLOBAL_CACHE_DIR="${VECTORCLICK_ZIG_GLOBAL_CACHE_DIR:-$cache_root/global}"
export ZIG_LOCAL_CACHE_DIR="${VECTORCLICK_ZIG_LOCAL_CACHE_DIR:-$cache_root/local}"
# Stable timestamps make same-source, same-toolchain Zig rebuilds easier to
# compare. Override only when deliberately changing the reproducible epoch.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1784246400}"

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/zig-windows.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF
cmake --build "$build_dir" --target VectorClick --parallel "${VECTORCLICK_BUILD_JOBS:-2}"
python3 "$script_dir/normalize-pe.py" "$build_dir/Vector Click.exe"

# Remove any stray standard-library probe output named `-`. It is not a
# project file or build input.
rm -f -- "$source_dir/-"

printf '%s\n' "Built: $build_dir/Vector Click.exe"

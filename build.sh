#!/usr/bin/env bash
# Builds NoFences into bin/linux and bin/windows: build.sh [linux|windows|all]
set -euo pipefail

target="${1:-all}"
case "$target" in
    linux|windows|all) ;;
    *) echo "usage: build.sh [linux|windows|all]" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "$0")" && pwd)"

if ! command -v cmake >/dev/null 2>&1; then
    echo "CMake isn't installed: install it and run this again." >&2
    exit 1
fi

# Ninja builds faster when it's installed; otherwise CMake's default generator is used.
generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi

build() {
    local name="$1"
    shift
    cmake -S "$root" -B "$root/build/$name" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release "$@"
    cmake --build "$root/build/$name"
}

if [[ "$target" == linux || "$target" == all ]]; then
    build linux
    echo "Built $root/bin/linux/NoFences"
fi

if [[ "$target" == windows || "$target" == all ]]; then
    if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        echo "The Windows build needs MinGW-w64: install x86_64-w64-mingw32-g++ (g++-mingw-w64-x86-64 on Debian and Ubuntu)." >&2
        exit 1
    fi
    build windows -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/mingw-w64.cmake"
    echo "Built $root/bin/windows/NoFences.exe"
fi

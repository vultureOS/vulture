#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds vultureOS: the loader, the kernel and the host tests. Afterwards,
# boot it with Meta/run.sh.
#
#   Meta/build.sh [options] [ninja targets...]
#
#   --build-dir DIR   build directory (default: <repo>/build)
#   --clean           delete the build directory first
#   --check           also run every test (unit, boot, crash, kernel)
#   -j N              run N jobs at once (default: one per CPU)
#
# Ninja targets name parts of the build or tests, for example "target"
# (the kernel), "test-kernel" or "bench".

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$root_dir/build"
clean=0
check=0
jobs=()
targets=()

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --clean) clean=1; shift ;;
        --check) check=1; shift ;;
        -j) jobs=(-j "$2"); shift 2 ;;
        -j*) jobs=("$1"); shift ;;
        -h|--help) sed -n '7,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "build.sh: unknown option '$1' (see --help)" >&2; exit 2 ;;
        *) targets+=("$1"); shift ;;
    esac
done

llvm_root="${VULTURE_LLVM_ROOT:-/opt/homebrew/opt/llvm@22}"
lld_root="${VULTURE_LLD_ROOT:-/opt/homebrew/opt/lld@22}"

# Name every missing tool at once, with the command that installs it.
missing=()
command -v cmake > /dev/null || missing+=(cmake)
command -v ninja > /dev/null || missing+=(ninja)
[ -x "$llvm_root/bin/clang++" ] || missing+=(llvm@22)
[ -x "$lld_root/bin/ld.lld" ] || missing+=(lld@22)
command -v "${QEMU:-qemu-system-aarch64}" > /dev/null || missing+=(qemu)
if [ ${#missing[@]} -gt 0 ]; then
    echo "build.sh: missing ${missing[*]}; install with: brew install ${missing[*]}" >&2
    exit 1
fi

if [ "$clean" -eq 1 ]; then
    rm -rf "$build_dir"
fi

if [ ! -f "$build_dir/build.ninja" ]; then
    cmake -G Ninja -S "$root_dir" -B "$build_dir" \
        -DVULTURE_LLVM_ROOT="$llvm_root" -DVULTURE_LLD_ROOT="$lld_root"
fi

ninja -C "$build_dir" ${jobs[@]+"${jobs[@]}"} ${targets[@]+"${targets[@]}"}

if [ "$check" -eq 1 ]; then
    ninja -C "$build_dir" ${jobs[@]+"${jobs[@]}"} check
fi

if [ ${#targets[@]} -eq 0 ]; then
    run_hint="Meta/run.sh"
    if [ "$build_dir" != "$root_dir/build" ]; then
        run_hint="Meta/run.sh --build-dir $build_dir"
    fi
    echo
    echo "Built vultureOS. Boot it with: $run_hint"
    echo "  (add --boot-args -v for a verbose boot, --help for more)"
fi

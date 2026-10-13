#!/usr/bin/env bash
# EdgeOfTimeRecomp on the PS5 - cross-build the ReXGlue SDK's runtime for the console.
#
#   ps5/tools/build-rexglue.sh              install the payload SDK (once), configure (once),
#                                           build rexruntime, then check its imports
#   ps5/tools/build-rexglue.sh --configure  configure again from scratch
#
# Checked out beside this repository (or named by the variables):
#
#   ../PS5_PayloadSDK   KongaTime/PS5_PayloadSDK, holding the pinned revision below
#                       (PS5_PAYLOAD_SDK_FORK)
#   ../rexglue-sdk      KongaTime/rexglue-sdk, the ReXGlue fork with the PS5 target, at
#                       the pinned revision or later (REXGLUE_SRC); the same checkout
#                       docs/BUILDING.md builds the desktop game with
#
# The SDK goes to .deps/native/ps5-payload-sdk, the build to build-ps5/rexglue, the
# libraries to ../rexglue-sdk/out/ps5-amd64. The last step lists every symbol the
# libraries need that neither they nor the modules a title links by default provide:
# anything there besides what the linker defines would stop the title's link.
#
# Needs clang 20 or later (llvm-config-20), CMake 3.25+, Ninja, wget, unzip, make.
#
# Copyright (C) 2026 KongaTime
# SPDX-License-Identifier: MIT
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
parent=$(dirname "$root")
sdk_fork=${PS5_PAYLOAD_SDK_FORK:-$parent/PS5_PayloadSDK}
rexglue=${REXGLUE_SRC:-$parent/rexglue-sdk}
cache=$root/.deps/native
sdk=$cache/ps5-payload-sdk
build=$root/build-ps5/rexglue

# The payload SDK fork at the revision PS5_RPCS3Title pins (1de8b37: pathconf, sbrk)
sdk_revision=1de8b37d583de476ad07cde9a0d562b75fb1be8f
# KongaTime/rexglue-sdk main: ReXGlue nightly-20261002-bd833a2a (what this repository's
# CI builds against, .github/workflows/build.yml) with the PS5 target
rexglue_revision=861df3a9a1cc40f5102332d5b9f07693b9c89e83

# The submodules the runtime builds from (thirdparty/CMakeLists.txt, Vulkan on, Tracy off)
submodules=(cli11 libmspack FFmpeg tomlplusplus simde xxHash spdlog fmt utfcpp imgui sdl3 o1heap
    vulkan-headers vulkan-memory-allocator spirv-headers spirv-tools glslang)

configure=false
for argument in "$@"; do
    case $argument in
        --configure) configure=true ;;
        *) echo "usage: ${0##*/} [--configure]" >&2; exit 2 ;;
    esac
done

step() { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

# The payload SDK, at its pin
if [[ ! -f $sdk/.ps5-sdk-revision || $(<"$sdk/.ps5-sdk-revision") != "$sdk_revision" ]]; then
    git -C "$sdk_fork" cat-file -e "$sdk_revision^{commit}" 2>/dev/null ||
        fail "no PS5_PayloadSDK checkout with $sdk_revision at $sdk_fork (set PS5_PAYLOAD_SDK_FORK)"
    step "installing the payload SDK at ${sdk_revision:0:7}"
    mkdir -p "$cache"
    sdk_tree=$(mktemp -d)
    trap 'rm -rf -- "$sdk_tree"' EXIT
    git -C "$sdk_fork" archive "$sdk_revision" | tar -x -C "$sdk_tree"
    bash "$sdk_tree/platform/tools/setup-sdk.sh" "$sdk" "$sdk_revision" "$cache"
fi
clang_major=$("$sdk/bin/prospero-llvm-config" --version | cut -d. -f1)
(( clang_major >= 20 )) || fail "the SDK found clang $clang_major; EdgeOfTimeRecomp needs 20 or later (install llvm-20)"

# The ReXGlue fork
[[ -f $rexglue/CMakeLists.txt ]] ||
    fail "no ReXGlue fork at $rexglue (git clone https://github.com/KongaTime/rexglue-sdk, or set REXGLUE_SRC)"
git -C "$rexglue" merge-base --is-ancestor "$rexglue_revision" HEAD 2>/dev/null ||
    fail "$rexglue does not contain the pinned fork revision ${rexglue_revision:0:7} (pull KongaTime/rexglue-sdk)"
for submodule in "${submodules[@]}"; do
    if [[ ! -e $rexglue/thirdparty/$submodule/.git ]]; then
        step "fetching ReXGlue's $submodule"
        GIT_LFS_SKIP_SMUDGE=1 git -C "$rexglue" submodule update --init --depth 1 "thirdparty/$submodule"
    fi
done

# Configure and build
mkdir -p "$build"
if $configure || [[ ! -f $build/build.ninja ]]; then
    rm -f "$build/CMakeCache.txt"
    flags="-march=znver2 -fno-omit-frame-pointer"
    step "configuring ReXGlue at $(git -C "$rexglue" rev-parse --short HEAD) (clang $clang_major)"
    cmake -S "$rexglue" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF \
        >"$build/configure.log" 2>&1 || {
        grep -n "CMake Error" -A6 "$build/configure.log" >&2
        fail "configure failed: $build/configure.log"; }
fi
step "building rexruntime"
ninja -C "$build" -k 0 rexruntime >"$build/build.log" 2>&1 ||
    fail "the build failed: $build/build.log ($(grep -c 'error:' "$build/build.log") errors)"

# What the libraries import that nothing provides. rex::ui::GetWindowedAppCreator
# is the app's (REX_DEFINE_APP), which the PS5 entry, rex_ps5_app_main, calls.
step "checking the runtime's imports"
nm=$sdk/bin/prospero-nm
out=$rexglue/out/ps5-amd64
work=$(mktemp -d)
trap 'rm -rf -- "${sdk_tree:-}" "$work"' EXIT
for archive in "$out"/*.a; do
    "$nm" -g --defined-only "$archive" 2>/dev/null | awk 'NF >= 3 { print $3 }'
done | sort -u >"$work/defined"
for archive in "$out"/*.a; do
    "$nm" -g --undefined-only "$archive" 2>/dev/null | awk 'NF && $NF !~ /:$/ { print $NF }'
done | sort -u >"$work/undefined"
{
    # The modules a title links by default (the SDK's compiler wrapper), the
    # system modules the runtime's PS5 drivers call (sound, the pad, the user
    # service: a title's link takes every SDK module it needs), and its static
    # libraries
    for module in libkernel_web libSceLibcInternal libc_stub_weak libkernel_stub_weak libSceNet \
            libSceAudioOut libScePad libSceUserService; do
        "$nm" -D --defined-only "$sdk/target/lib/$module.so" | awk '{ print $NF }'
    done
    # (not libc.a: a title takes its C library from the console's modules and
    # the platform layer, so ReXGlue's ffs failed the title's link while libc.a
    # had it)
    for library in libc++.a libc++abi.a libc++experimental.a libunwind.a libpthread.a libps5platform.a; do
        "$nm" -g --defined-only "$sdk/target/lib/$library" 2>/dev/null | awk 'NF >= 3 { print $3 }'
    done
} | sort -u >"$work/console"
# __emutls_get_address is Clang's builtins', which a title's link recipe takes;
# __real_abort is what a title's --wrap=abort gives the fault records'
# __wrap_abort (the fork's src/core/abort_prospero.cpp)
comm -23 "$work/undefined" "$work/defined" | comm -23 - "$work/console" |
    grep -vxE '_GLOBAL_OFFSET_TABLE_|__dso_handle|__emutls_get_address|__real_abort|_ZN3rex2ui21GetWindowedAppCreatorEv' \
    >"$work/unprovided" || true
# What the platform layer has as ps5_<name>: a title binds those (--defsym), in
# PS5_Vulkan's recipe or its own list (PS5_EdgeOfTimeTitle's reeot/reeot.cmake);
# link-title.sh refuses a title that leaves one unbound
"$nm" -g --defined-only "$sdk/target/lib/libps5platform.a" 2>/dev/null |
    awk 'NF >= 3 && $3 ~ /^ps5_/ { print substr($3, 5) }' | sort -u >"$work/bindable"
comm -12 "$work/unprovided" "$work/bindable" >"$work/bind"
comm -23 "$work/unprovided" "$work/bindable" >"$work/missing"
if [[ -s $work/bind ]]; then
    echo "imported, and bound by a title to the platform layer's ps5_ functions:" $(cat "$work/bind")
fi
if [[ -s $work/missing ]]; then
    echo "imported, and provided by nothing a title links:" >&2
    sed 's/^/  /' "$work/missing" >&2
    fail "$(wc -l <"$work/missing") unresolved symbols"
fi
step "rexruntime built for the PS5 ($out), every import resolved"

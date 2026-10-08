#!/usr/bin/env bash
# Copyright 2020 Alibaba Group Holding Limited.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ---------------------------------------------------------------------------
# F008 odps extension — authoritative SDK-ON validation for x86_64 Linux / CI
# ---------------------------------------------------------------------------
#
# WHY THIS SCRIPT EXISTS
#   The ODPS data-plane SDK (aliyun-odps-sdk-cpp) only builds on x86_64 Linux:
#   util/crc32c.cpp uses SSE4.2/cpuid inline asm (guarded by third_party/
#   odps-sdk.patch, which is a no-op on x86_64 and unblocks arm64 dev builds).
#   Local arm64 verification (a standalone glue harness) already passed; this
#   script is the production-faithful end-to-end check that a real x86_64 host
#   or CI runner should execute. It builds the FULL core + extension with
#   NEUG_WITH_ODPS_SDK=ON and asserts the three integration constraints:
#     C1 ExternalProject build produces the four SDK static libs;
#     C2 crc32c arch guard (SDK compiles on this arch);
#     C3 ABI seam — inner glue is ABI=0, core libneug leaks no SDK/Arrow symbol.
#
# USAGE
#   specs/008-odps-storage-extension/validate_sdk_x86_64.sh
#
# ENV OVERRIDES
#   BUILD_DIR   build tree to use                (default: build-sdk-on)
#   JOBS        parallelism, capped to cores      (default: nproc / hw.physicalcpu)
#   NEUG_ODPS_SDK_SOURCE_DIR  SDK source checkout (default: vendored submodule)
#   KEEP_GOING  set to 1 to not exit on first failure (default: 0)
#
# EXIT CODE: 0 = all checks passed, non-zero = at least one failure.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-build-sdk-on}"
KEEP_GOING="${KEEP_GOING:-0}"

# --- parallelism: cap to physical cores, NEVER bare -j (see AGENTS.md) -------
if [[ "$(uname -s)" == "Darwin" ]]; then
  CORES="$(sysctl -n hw.physicalcpu 2>/dev/null || echo 4)"
else
  CORES="$(nproc 2>/dev/null || echo 4)"
fi
JOBS="${JOBS:-$CORES}"

FAILURES=0
info()  { printf '\e[34m[INFO]\e[0m %b\n'  "$*"; }
pass()  { printf '\e[32m[PASS]\e[0m %b\n'  "$*"; }
fail()  { printf '\e[31m[FAIL]\e[0m %b\n'  "$*"; FAILURES=$((FAILURES + 1)); \
          [[ "$KEEP_GOING" == "1" ]] || exit 1; }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

# ===========================================================================
# 0. Preconditions: this validation is only meaningful on x86_64 Linux + GCC.
# ===========================================================================
info "Repo root: $REPO_ROOT"
info "Build dir: $BUILD_DIR  |  Jobs: $JOBS"

ARCH="$(uname -m)"
OS="$(uname -s)"
[[ "$OS" == "Linux" ]]     || fail "expected Linux, found '$OS' (the SDK data plane is Linux-only)"
[[ "$ARCH" == "x86_64" ]]  || fail "expected x86_64, found '$ARCH' (SDK util/crc32c.cpp needs x86 SSE4.2; use an x86_64 host/CI runner)"

command -v cmake >/dev/null 2>&1 || fail "cmake not found in PATH"
command -v g++   >/dev/null 2>&1 || fail "g++ not found in PATH (the SDK pins GNU-only flags)"

CMAKE_VER="$(cmake -E capabilities >/dev/null 2>&1 && cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
info "cmake version: ${CMAKE_VER:-unknown} (need >= 3.22)"

CXX_ID="$(g++ -dumpversion >/dev/null 2>&1 && echo GNU || echo unknown)"
info "C++ compiler id: $CXX_ID ($(g++ --version 2>/dev/null | head -1))"

# ===========================================================================
# 1. SDK submodule must be present (ExternalProject consumes its source tree).
# ===========================================================================
SDK_DIR="${NEUG_ODPS_SDK_SOURCE_DIR:-$REPO_ROOT/third_party/aliyun-odps-sdk-cpp}"
if [[ ! -f "$SDK_DIR/CMakeLists.txt" ]]; then
  info "Initializing SDK submodule at third_party/aliyun-odps-sdk-cpp ..."
  git submodule update --init --recursive third_party/aliyun-odps-sdk-cpp \
    || fail "could not init the SDK submodule (network? set NEUG_ODPS_SDK_SOURCE_DIR)"
fi
[[ -f "$SDK_DIR/CMakeLists.txt" ]] || fail "SDK source not found at '$SDK_DIR'"
info "SDK source: $SDK_DIR"

# The crc32c arch-guard patch ships at third_party/odps-sdk.patch and is applied
# idempotently by build_odps_sdk_as_third_party(); on x86_64 it is a no-op.
[[ -f "$REPO_ROOT/third_party/odps-sdk.patch" ]] \
  || fail "third_party/odps-sdk.patch missing (C2 crc32c arch guard)"

# ===========================================================================
# 2. Configure with the SDK enabled.
# ===========================================================================
info "Configuring NEUG_WITH_ODPS_SDK=ON ..."
cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXTENSIONS="odps" \
  -DBUILD_TEST=ON \
  -DNEUG_WITH_ODPS_SDK=ON \
  ${NEUG_ODPS_SDK_SOURCE_DIR:+-DNEUG_ODPS_SDK_SOURCE_DIR="$NEUG_ODPS_SDK_SOURCE_DIR"} \
  || fail "cmake configure failed"

# ===========================================================================
# 3. Build core + extension + unit tests. This drives the ExternalProject that
#    compiles the four SDK static libs (C1) and the ABI=0 glue (C3).
# ===========================================================================
info "Building neug + neug_odps_extension + odps_extension_test (-j$JOBS) ..."
cmake --build "$BUILD_DIR" \
  --target neug neug_odps_extension odps_extension_test \
  --parallel "$JOBS" \
  || fail "build failed"

# --- C1: the four SDK static libs were produced by the ExternalProject -------
SDK_BUILD="$BUILD_DIR/third_party/aliyun-odps-sdk-cpp-build"
for lib in libodps_sdk_common_static.a libodps_sdk_core_static.a \
           libodps_sdk_tunnel_static.a libmax_storage_api.a; do
  check "C1: SDK static lib built — $lib" "[[ -s '$SDK_BUILD/lib/$lib' ]]"
done

# --- artifacts exist --------------------------------------------------------
EXT_SO="$(find "$BUILD_DIR" -name 'libodps.neug_extension' | head -1)"
GLUE_A="$(find "$BUILD_DIR" -name 'libodps_sdk_glue.a' | head -1)"
LIBNEUG="$(find "$BUILD_DIR" -maxdepth 4 -name 'libneug.so' -o -maxdepth 4 -name 'libneug.dylib' | head -1)"
check "extension shared object built" "[[ -n '$EXT_SO' && -s '$EXT_SO' ]]"
check "inner glue static lib built"   "[[ -n '$GLUE_A' && -s '$GLUE_A' ]]"
check "core libneug built"            "[[ -n '$LIBNEUG' && -s '$LIBNEUG' ]]"

# ===========================================================================
# 4. C3 — ABI seam assertions.
# ===========================================================================
if [[ -n "$GLUE_A" ]]; then
  # 4a. The glue must be compiled with the pre-C++11 ABI (=0): ABI=1 code emits
  #     std::__cxx11::basic_string symbols, ABI=0 emits none.
  CXX11_SYMS="$(nm "$GLUE_A" 2>/dev/null | grep -c '__cxx11' || true)"
  check "C3: glue is ABI=0 (no std::__cxx11 symbols; found $CXX11_SYMS)" \
        "[[ '$CXX11_SYMS' == '0' ]]"

  # 4b. The C seam symbols must be unmangled (extern "C").
  for sym in odps_glue_connect odps_glue_disconnect odps_glue_sniff_schema \
             odps_glue_schema_free odps_glue_free_string; do
    check "C3: extern \"C\" seam symbol present & unmangled — $sym" \
          "nm '$GLUE_A' 2>/dev/null | grep -qE ' T $sym\$'"
  done
fi

# 4c. Core libneug must not export/define any SDK or Arrow symbol (RTLD_LOCAL
#     isolation: the SDK/Arrow stay confined to the extension).
if [[ -n "$LIBNEUG" ]]; then
  LEAK="$(nm -D --defined-only "$LIBNEUG" 2>/dev/null | grep -icE 'apsara|arrow::|MaxStorageApi' || true)"
  check "C3: libneug leaks no SDK/Arrow symbol (found $LEAK)" "[[ '$LEAK' == '0' ]]"
fi

# 4d. The extension .so must have been compiled with ODPS_SDK_ENABLE_ARROW.
#     The sniff path only exists under that macro; assert the glue seam is
#     referenced by the extension object set.
if [[ -n "$EXT_SO" ]]; then
  check "extension references the glue C seam (odps_glue_connect)" \
        "nm -D '$EXT_SO' 2>/dev/null | grep -q 'odps_glue_connect' || nm '$EXT_SO' 2>/dev/null | grep -q 'odps_glue_connect'"
fi

# ===========================================================================
# 5. Unit tests (SDK-ON build; SDK-gated cases exercise the glue path).
# ===========================================================================
TEST_BIN="$(find "$BUILD_DIR" -name 'odps_extension_test' -type f | head -1)"
if [[ -n "$TEST_BIN" ]]; then
  info "Running $TEST_BIN ..."
  if "$TEST_BIN"; then pass "odps unit tests (SDK-ON)"; else fail "odps unit tests failed"; fi
else
  fail "odps_extension_test binary not found"
fi

# ===========================================================================
# 6. OPTIONAL live smoke against a real MaxCompute project.
#    The v1 read path (Storage API split reader, task T106) is not implemented
#    yet, so a full `LOAD FROM "odps://..."` e2e is not available. When it
#    lands, gate a real query here behind the credentials below.
# ===========================================================================
if [[ "${ODPS_LIVE_SMOKE:-0}" == "1" ]]; then
  : "${ODPS_ACCESS_ID:?ODPS_LIVE_SMOKE=1 needs ODPS_ACCESS_ID}"
  : "${ODPS_ACCESS_KEY:?ODPS_LIVE_SMOKE=1 needs ODPS_ACCESS_KEY}"
  : "${ODPS_ENDPOINT:?ODPS_LIVE_SMOKE=1 needs ODPS_ENDPOINT}"
  : "${ODPS_PROJECT:?ODPS_LIVE_SMOKE=1 needs ODPS_PROJECT}"
  info "TODO(T106): run a live 'LOAD FROM \"odps://\$ODPS_PROJECT/<table>\" RETURN *' here."
  info "Credentials present; skipping live query until the Storage API reader lands."
fi

# ===========================================================================
# Summary
# ===========================================================================
if [[ "$FAILURES" == "0" ]]; then
  pass "ALL CHECKS PASSED (x86_64 SDK-ON validation)"
  exit 0
else
  fail "$FAILURES check(s) failed"
  exit 1
fi

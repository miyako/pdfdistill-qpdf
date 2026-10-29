#!/usr/bin/env bash
# Build a universal (arm64 + x86_64) macOS binary.
#
# vcpkg builds one architecture per triplet, so each architecture is
# configured/built separately against its own static dependency tree and the
# two executables are merged with lipo.
#
# Requires: VCPKG_ROOT, CMake >= 3.21, Ninja, Xcode command line tools.
set -euo pipefail

: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg checkout}"
cd "$(dirname "$0")/.."
BUILD_ROOT=${BUILD_ROOT:-build}
RUN_TESTS=${RUN_TESTS:-1}

for arch in arm64 x64; do
  osx_arch=$arch; [ "$arch" = x64 ] && osx_arch=x86_64
  cmake -S . -B "$BUILD_ROOT/$arch" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET="$arch-osx-pdfdistill" \
    -DCMAKE_OSX_ARCHITECTURES="$osx_arch"
  cmake --build "$BUILD_ROOT/$arch"
done

# Tests run natively (arm64 on Apple silicon, x64 on Intel or via Rosetta).
if [ "$RUN_TESTS" = 1 ]; then
  native=x64; [ "$(uname -m)" = arm64 ] && native=arm64
  ctest --test-dir "$BUILD_ROOT/$native" --output-on-failure
fi

mkdir -p "$BUILD_ROOT/universal"
lipo -create -output "$BUILD_ROOT/universal/pdfdistill-qpdf" \
  "$BUILD_ROOT/arm64/pdfdistill-qpdf" "$BUILD_ROOT/x64/pdfdistill-qpdf"
strip -x "$BUILD_ROOT/universal/pdfdistill-qpdf" || true
lipo -info "$BUILD_ROOT/universal/pdfdistill-qpdf"
otool -L "$BUILD_ROOT/universal/pdfdistill-qpdf"

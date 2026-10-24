#!/usr/bin/env bash
#
# Build script for XNU-4903 on OnePlus 2 (MSM8994) using Xcode 10.3 toolchain.
#
set -o errexit
set -o pipefail

REPO_ROOT="/Users/veerhooda/oneplus2"
XNU_SRC="${REPO_ROOT}/xnu/xnu-4903.270.47"
XCODE_ROOT="/Users/veerhooda/xcode-10.3/Xcode.app/Contents/Developer"
TOOLCHAIN="${XCODE_ROOT}/Toolchains/XcodeDefault.xctoolchain/usr/bin"
SDK="${XCODE_ROOT}/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.14.sdk"

export PATH="${REPO_ROOT}/tools:${TOOLCHAIN}:/usr/bin:/bin:/usr/sbin:/sbin"
unset DEVELOPER_DIR
export XCRUN="${REPO_ROOT}/tools/xcrun"

BUILD_DIR="${XNU_SRC}/BUILD"
mkdir -p "${BUILD_DIR}/obj" "${BUILD_DIR}/dst"

echo "[BUILD] Starting serial XNU build (-j1) for DEVELOPMENT_ARM64_MSM8994..."
make -C "${XNU_SRC}" \
  -j1 \
  MAKEJOBS="--jobs=1" \
  VERBOSE=YES \
  SRCROOT="${XNU_SRC}" \
  OBJROOT="${BUILD_DIR}/obj" \
  DSTROOT="${BUILD_DIR}/dst" \
  SDKROOT="${SDK}" \
  ARCH_CONFIGS=ARM64 \
  KERNEL_CONFIGS=DEVELOPMENT \
  MACHINE_CONFIGS=MSM8994 \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
  XCRUN="${REPO_ROOT}/tools/xcrun" \
  DO_CTFCONVERT=0 \
  DO_CTFMERGE=0 \
  "$@" 2>&1 | tee "${REPO_ROOT}/build.log"

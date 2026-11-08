#!/usr/bin/env bash
#
# Build script for OP2Metal.bundle and metal_test diagnostic tool.
# Targets iOS 12.0 ARM64 using Xcode 10.3 iPhoneOS12.4 SDK.
#
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="/Users/veerhooda/xcode-10.3/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS12.4.sdk"

echo "[BUILD] Compiling OP2Metal bundle..."
mkdir -p "${DIR}/build/OP2Metal.bundle"

clang -target arm64-apple-ios12.0 \
      -isysroot "${SDK}" \
      -bundle \
      -fobjc-arc \
      -O2 \
      -Wall \
      -Wno-deprecated-declarations \
      "${DIR}/OP2Metal.m" "${DIR}/op2_metallib.c" \
      -framework Foundation \
      -framework Metal \
      -framework IOKit \
      -o "${DIR}/build/OP2Metal.bundle/OP2Metal"

cp "${DIR}/Info.plist" "${DIR}/build/OP2Metal.bundle/Info.plist"
cp "${DIR}/OP2Metal.clients" "${DIR}/build/OP2Metal.bundle/OP2Metal.clients"

echo "[SIGN] Signing OP2Metal.bundle (ad-hoc)..."
codesign -s - "${DIR}/build/OP2Metal.bundle/OP2Metal"

echo "[BUILD] Compiling metal_test diagnostic utility..."
clang -target arm64-apple-ios12.0 \
      -isysroot "${SDK}" \
      -fobjc-arc \
      -O2 \
      -Wall \
      -Wno-deprecated-declarations \
      "${DIR}/metal_test.m" \
      -framework Foundation \
      -framework Metal \
      -framework IOKit \
      -o "${DIR}/build/metal_test"

codesign -s - -i com.apple.tzinit --force "${DIR}/build/metal_test"

echo "[SUCCESS] Built and signed OP2Metal.bundle and metal_test at: ${DIR}/build"

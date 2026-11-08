#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -ne 1 ]; then
    echo 'usage: bash tools/build-sweet-backboard-wrapper.sh OUTPUT_DIRECTORY' >&2
    exit 2
fi
repo=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1/usr/libexec"
out=$(cd "$1" && pwd)
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
"$cc" -target arm64-apple-ios16.0 -isysroot "$sdk" -O2 -Wall -Wextra \
    "$repo/userland/gpu/sweet-backboard-wrapper.c" \
    -o "$out/usr/libexec/backboardd"
codesign --force --sign - --timestamp=none "$out/usr/libexec/backboardd"
codesign --verify "$out/usr/libexec/backboardd"
[ "$(xcrun lipo -archs "$out/usr/libexec/backboardd")" = arm64 ]
(cd "$out" && shasum -a 256 usr/libexec/backboardd > SHA256SUMS)

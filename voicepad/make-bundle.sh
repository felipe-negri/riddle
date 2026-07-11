#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
BIN=../quill/build/voicepad
[ -f "$BIN" ] || { echo "build first: ../quill/build.sh" >&2; exit 1; }
[ -f ../quill/build/libquill.so ] || { echo "missing ../quill/build/libquill.so" >&2; exit 1; }
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
if [ -x android/build.sh ] && [ -d "$SDK" ]; then
    android/build.sh
fi
rm -rf dist/voicepad
mkdir -p dist/voicepad
install -m 755 "$BIN" dist/voicepad/voicepad
install -m 755 ../quill/build/libquill.so dist/voicepad/
install -m 755 scripts/appload-launch.sh scripts/voicepad-takeover.sh dist/voicepad/
install -m 644 external.manifest.json dist/voicepad/
[ -f icon.png ] && install -m 644 icon.png dist/voicepad/ || true
[ -f android/build/voicepad.apk ] && install -m 644 android/build/voicepad.apk dist/voicepad/ || true
echo "staged: $(du -sh dist/voicepad | cut -f1) in dist/voicepad/"

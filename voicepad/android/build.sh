#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
ANDROID_JAR=$(find "$SDK/platforms" -mindepth 2 -maxdepth 2 -name android.jar | sort -V | tail -n1)
BUILD_TOOLS=$(find "$SDK/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n1)
AAPT2="$BUILD_TOOLS/aapt2"
D8="$BUILD_TOOLS/d8"
ZIPALIGN="$BUILD_TOOLS/zipalign"
APKSIGNER="$BUILD_TOOLS/apksigner"

for file in "$ANDROID_JAR" "$AAPT2" "$D8" "$ZIPALIGN" "$APKSIGNER"; do
    [ -e "$file" ] || { echo "missing Android build dependency: $file" >&2; exit 1; }
done

rm -rf build
mkdir -p build/compiled build/classes build/dex

"$AAPT2" compile --dir res -o build/compiled/resources.zip
"$AAPT2" link \
    -I "$ANDROID_JAR" \
    --manifest AndroidManifest.xml \
    --min-sdk-version 23 \
    --target-sdk-version 35 \
    --version-code 5 \
    --version-name 0.4.0 \
    -o build/voicepad-unsigned.apk \
    build/compiled/resources.zip

mapfile -t SOURCES < <(find src -name '*.java' -type f | sort)
javac -encoding UTF-8 -source 8 -target 8 -Xlint:-options \
    -classpath "$ANDROID_JAR" \
    -d build/classes \
    "${SOURCES[@]}"

jar cf build/classes.jar -C build/classes .
"$D8" --lib "$ANDROID_JAR" --min-api 23 --output build/dex build/classes.jar
(
    cd build/dex
    zip -q -u ../voicepad-unsigned.apk classes.dex
)
"$ZIPALIGN" -f 4 build/voicepad-unsigned.apk build/voicepad-aligned.apk

KEYSTORE=${VOICEPAD_KEYSTORE:-$HOME/.android/voicepad-debug.keystore}
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    keytool -genkeypair -noprompt \
        -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias voicepad -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Voicepad Debug,O=Remagic,C=CA" >/dev/null
fi
"$APKSIGNER" sign \
    --ks "$KEYSTORE" --ks-key-alias voicepad \
    --ks-pass pass:android --key-pass pass:android \
    --out build/voicepad.apk build/voicepad-aligned.apk
"$APKSIGNER" verify --verbose build/voicepad.apk >/dev/null

printf 'built: android/build/voicepad.apk (%s)\n' "$(du -h build/voicepad.apk | cut -f1)"

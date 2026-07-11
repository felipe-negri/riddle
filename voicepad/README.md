# Voicepad

Voicepad is a reMarkable Paper Pro takeover app with a native Android companion.
Speech and keyboard input share one persisted editable document and cursor.
Remote finger ink and tablet pen annotations are composited over that document.

## Install and pair the Android companion

1. Start Voicepad on the tablet.
2. Put the Android phone and tablet on the same Wi-Fi.
3. Scan the QR shown by Voicepad.
4. Tap **Download the Android app** and open `voicepad.apk`.
5. If prompted, allow the browser to install unknown apps.
6. Return to the scanned page and tap **Pair and open the installed app**.
7. Grant Voicepad microphone permission and tap **Start microphone**.

The app can also find Voicepad by UDP broadcast or accept the tablet address
manually. The companion is signed with the local Voicepad debug key at
`~/.android/voicepad-debug.keystore`; keep that key to install future builds as
updates over the existing app.

The companion supports:

- continuous native Android speech recognition inserted at the shared cursor;
- live Bluetooth keyboard forwarding while the app is open, including arrow
  cursor movement, Enter, Tab, Backspace, Delete, Home, and End;
- finger drawing and erasing from the phone;
- Android-selected Bluetooth microphones/headsets.

This is a LAN-only development build. Port 7777 is unauthenticated, so use it
only on a trusted network.

## Document and ink model

Voicepad stores Unicode code points in an editable linear document with a
cursor and deterministic fixed-grid layout. The document and cursor persist in
`voicepad-document.txt` and `voicepad-cursor.txt`. They are also available over
`GET /document` and `GET /state`, allowing later automation to edit structured
text directly rather than recovering it from screenshots.

Rendering uses separate static-paper, generated-text, and manuscript-ink
layers. Pen input is retained as document-positioned, tool-tagged vector
segments. The eraser currently renders as a persistent white mask, but remains
identified as an eraser gesture so it can later become a strike-through or
candidate-deletion annotation without changing the text representation.

Fast interaction and cleanup are split into two phases. Pixel diffs and the
visible cursor use `quill_swap_mono_fast()` immediately. The cursor is hidden
and debounced during bursts, so intermediate arrow/key positions never reach
the panel. Retired changes are unioned into one exact focus rectangle; after
400 ms idle it receives at most one `quill_swap_mono_quality()` call. This path
always uses `full=0`, rejects regions taller than 192 pixels, and can therefore
never promote background cleanup into a whole-screen refresh.

## Build

Prerequisites are a JDK and Android SDK. The build script uses `aapt2`, `d8`,
`zipalign`, and `apksigner` directly, so Gradle is not required.

```sh
cd riddle/voicepad/android
./build.sh
```

The signed APK is written to `android/build/voicepad.apk`. To cross-build and
stage the complete tablet bundle:

```sh
cd riddle/quill
./build.sh
cd ../voicepad
./make-bundle.sh
```

The bundle appears in `riddle/voicepad/dist/voicepad/`, including
`voicepad.apk`, which the tablet serves from `GET /voicepad.apk`.

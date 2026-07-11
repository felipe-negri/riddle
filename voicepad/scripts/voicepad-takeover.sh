#!/bin/bash
restore() {
    rm -f /tmp/epframebuffer.lock
    systemctl start xochitl
}
if [ -z "${REMAGIC_SESSION:-}" ]; then
    trap restore EXIT INT TERM
    systemctl stop xochitl
fi
HERE=$(cd "$(dirname "$0")" && pwd)
rm -f /tmp/epframebuffer.lock
[ -z "${REMAGIC_SESSION:-}" ] && sleep 1
cd "$HERE"
LD_LIBRARY_PATH="$HERE:/home/root/quill:/usr/lib/plugins/scenegraph" \
    HOME=/home/root \
    "$HERE/voicepad"
echo "voicepad-takeover: closed ($?), restoring xochitl"

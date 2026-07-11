#!/bin/sh
HERE=$(cd "$(dirname "$0")" && pwd)
systemctl is-active --quiet voicepad-takeover && exit 0
systemd-run --unit=voicepad-takeover --collect /bin/bash "$HERE/voicepad-takeover.sh"
exit 0

#!/bin/sh
# Flash the LilyGO keypads firmware while Hammerspoon is running.
# Hammerspoon holds the serial port, so release it first and take it back
# afterwards (even if the upload fails).
# Usage: services/keypads/tools/flash.sh [pio env, default keypads]
set -u
ENV="${1:-keypads}"
FW="$(cd "$(dirname "$0")/../src/firmware" && pwd)"

perl -e 'alarm 10; exec @ARGV' hs -c 'return keypads and keypads.lilygo.pause() or "keypads not loaded"'
pio run -d "$FW" -e "$ENV" -t upload
STATUS=$?
perl -e 'alarm 10; exec @ARGV' hs -c 'return keypads and keypads.lilygo.resume() or "keypads not loaded"'
exit $STATUS

#!/bin/bash
# Convert Apollo's UI sound effects from mp3 to the firmware's format:
# Ogg/Opus, mono 16 kHz, 48 kbps, 60 ms frames, peak-normalized to -20 dBFS.
#
# Usage: ./scripts/convert_apollo_sounds.sh <mp3-dir>
#
# Converts every known effect found in <mp3-dir> (skips the missing ones) and
# generates pitch-shifted _v2/_v3 variants (±6%, duration preserved) for the
# frequent effects, so repeated plays never sound machine-identical.
#
# After running, regenerate the language header and force a reconfigure:
#   python3 scripts/gen_lang.py --language es-ES --output main/assets/lang_config.h
#   touch main/CMakeLists.txt
set -euo pipefail

SRC=${1:?usage: $0 <mp3-dir>}
DST="$(cd "$(dirname "$0")/.." && pwd)/main/assets/common"
TARGET_PEAK_DB=-20

FFMPEG=$(command -v ffmpeg || true)
if [ -z "$FFMPEG" ]; then
  # This machine's brew is broken; ffmpeg lives as a pip wheel in the IDF venv.
  FFMPEG=$(ls "$HOME"/.espressif/python_env/*/lib/python*/site-packages/static_ffmpeg/bin/*/ffmpeg 2>/dev/null | head -1)
fi
[ -n "$FFMPEG" ] || { echo "ffmpeg not found (pip install static-ffmpeg)"; exit 1; }

# name:variants — 3 means base + _v2 (pitch down) + _v3 (pitch up)
SOUNDS="exclamation:1 low_battery:1 popup:1 success:1 vibration:1 \
        listen_start:3 listen_end:3 mode_switch:3 speech_done:3"

gain_for() {
  local max
  max=$("$FFMPEG" -hide_banner -i "$1" -af volumedetect -f null - 2>&1 |
    sed -n 's/.*max_volume: \(-\{0,1\}[0-9.]*\) dB/\1/p')
  python3 -c "print(f'{$TARGET_PEAK_DB - ($max):.1f}')"
}

encode() { # in, out, gain-db, extra-filter (or empty)
  local filter="volume=${3}dB"
  [ -n "${4:-}" ] && filter="$filter,$4"
  "$FFMPEG" -hide_banner -loglevel error -y -i "$1" -af "$filter" \
    -c:a libopus -b:a 48k -ac 1 -ar 16000 -frame_duration 60 "$2"
}

for entry in $SOUNDS; do
  name=${entry%%:*} variants=${entry##*:}
  in="$SRC/$name.mp3"
  [ -f "$in" ] || continue
  gain=$(gain_for "$in")
  encode "$in" "$DST/$name.ogg" "$gain" ""
  echo "$name: gain ${gain}dB"
  if [ "$variants" = 3 ]; then
    encode "$in" "$DST/${name}_v2.ogg" "$gain" "asetrate=16000*0.943,aresample=16000,atempo=1.0605"
    encode "$in" "$DST/${name}_v3.ogg" "$gain" "asetrate=16000*1.059,aresample=16000,atempo=0.9443"
    echo "$name: _v2 (pitch -6%), _v3 (pitch +6%)"
  fi
done

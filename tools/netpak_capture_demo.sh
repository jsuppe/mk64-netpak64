#!/usr/bin/env bash
# Run inside the ffmpeg-enabled harness container after launching harness4.
# Recording starts on actual driver telemetry, not a guessed menu timeout.
set -euo pipefail
OUT=${OUT:-/work/np64out/drive-v77-final}
CAPTURE_SECONDS=${CAPTURE_SECONDS:-240}
READY_TIMEOUT=${READY_TIMEOUT:-210}
export DISPLAY=${DISPLAY:-:77}

for ((seconds=0; seconds<READY_TIMEOUT; seconds+=2)); do
  if grep -qi 'io wW 0020 <- b1' "$OUT/a4.log" 2>/dev/null; then
    ready=1
    break
  fi
  sleep 2
done
[[ ${ready:-0} == 1 ]] || { echo 'capture: no driver telemetry before timeout' >&2; exit 1; }

mapfile -t windows < <(xdotool search --name mk64 | sort -nu)
[[ ${#windows[@]} == 4 ]] || { echo 'capture: expected four emulator windows' >&2; exit 1; }
eval "$(xdotool getwindowgeometry --shell "${windows[0]}")"
columns=2
gap=8
for index in "${!windows[@]}"; do
  xdotool windowmove "${windows[index]}" \
    $(( (index % columns) * (WIDTH + gap) )) \
    $(( (index / columns) * (HEIGHT + gap) ))
done
width=$((WIDTH * 2 + gap))
height=$((HEIGHT * 2 + gap))
echo "capture: racing, recording ${width}x${height} for ${CAPTURE_SECONDS}s"
ffmpeg -nostdin -loglevel warning -f x11grab -framerate 15 \
  -video_size "${width}x${height}" -i "${DISPLAY}.0" -t "$CAPTURE_SECONDS" \
  -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p -movflags +faststart \
  "$OUT/race-raw.mp4"
ffprobe -v error -show_entries format=duration -of default=nw=1 "$OUT/race-raw.mp4"
echo 'capture: file finalized; visual review and hash comparison still required'

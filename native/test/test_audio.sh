#!/usr/bin/env bash
# Host test of the wind synthesis (src/audio.c compiled with AUDIO_SYNTH_ONLY): levels,
# interior, thunder, fade, and a dropout stress of AUDIO_MINUTES (default 10) per wind level.
#   AUDIO_OUT=dir keeps the rendered WAV files; otherwise a temporary directory is used.
#   AUDIO_DEVICE_TEST=1 also runs the real waveOut layer against a fake real-time device
#   under wine (needs x86_64-w64-mingw32-gcc and wine; ~40 s): no underrun allowed without
#   stalls and with the worker stalled 180 ms every 2 s.
set -euo pipefail
cd "$(dirname "$0")"
if [ -n "${AUDIO_OUT:-}" ]; then
    OUT=$AUDIO_OUT
    mkdir -p "$OUT"
else
    OUT=$(mktemp -d)
    trap 'rm -rf "$OUT"' EXIT
fi
"${HOSTCC:-gcc}" -std=c11 -pedantic -Wall -Wextra -Werror -O2 -o "$OUT/audio_test" audio_test.c -lm
"$OUT/audio_test" "$OUT" "${AUDIO_MINUTES:-10}"

if [ "${AUDIO_DEVICE_TEST:-0}" = 1 ]; then
    x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -municode \
        -o "$OUT/audio_device_test.exe" audio_device_test.c -lwinmm
    WINEDEBUG=-all wine "$OUT/audio_device_test.exe" 15 0 1000
    WINEDEBUG=-all wine "$OUT/audio_device_test.exe" 20 180 2000
    echo "audio device layer: OK"
fi

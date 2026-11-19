#!/usr/bin/env bash
# Host test of the wind synthesis (src/audio.c compiled with AUDIO_SYNTH_ONLY).
#   AUDIO_OUT=dir keeps the rendered WAV files; otherwise a temporary directory is used.
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
"$OUT/audio_test" "$OUT"

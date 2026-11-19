#!/usr/bin/env bash
# Host tests for the native engine: builds the C parity harness with host gcc and diffs its
# output against the reference web code run in node. Exits non-zero on any difference.
#   native/test/run.sh [SEEDS] [DAYS] [LONG_DAYS]
set -euo pipefail
cd "$(dirname "$0")"
SEEDS=${1:-320}
DAYS=${2:-64}
LONG=${3:-3650}
# PARITY_OUT=dir keeps the outputs for inspection; otherwise a temporary directory is used.
if [ -n "${PARITY_OUT:-}" ]; then
    OUT=$PARITY_OUT
    mkdir -p "$OUT"
else
    OUT=$(mktemp -d)
    trap 'rm -rf "$OUT"' EXIT
fi

CC=${HOSTCC:-gcc}
FLAGS="-std=c11 -pedantic -Wall -Wextra -Werror -O2 -ffp-contract=off"
"$CC" $FLAGS -o "$OUT/parity" parity.c ../src/engine.c -lm

"$CC" $FLAGS -o "$OUT/test_engine" test_engine.c ../src/engine.c -lm
"$OUT/test_engine"

node gen_seeds.js "$SEEDS" > "$OUT/seeds.txt"
node parity.js "$OUT/seeds.txt" "$DAYS" "$LONG" > "$OUT/js.txt"
"$OUT/parity" "$OUT/seeds.txt" "$DAYS" "$LONG" > "$OUT/c.txt"

lines=$(wc -l < "$OUT/js.txt")
seeds=$(wc -l < "$OUT/seeds.txt")
if cmp -s "$OUT/js.txt" "$OUT/c.txt"; then
    echo "engine parity: OK ($seeds seeds x $DAYS days, long run $LONG days, $lines lines identical)"
else
    ndiff=$(diff "$OUT/js.txt" "$OUT/c.txt" | grep -c '^[<>]' || true)
    echo "engine parity: FAILED ($ndiff differing lines of $lines)" >&2
    diff "$OUT/js.txt" "$OUT/c.txt" | head -40 >&2
    exit 1
fi

# further host test suites (store round trip, ...) register here
for t in ./test_*.sh; do
    [ -e "$t" ] || continue
    bash "$t"
done

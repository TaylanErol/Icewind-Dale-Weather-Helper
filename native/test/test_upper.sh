#!/usr/bin/env bash
# Seed upper-casing parity: alm_normalize_seed vs node's trim().toUpperCase() on every
# Unicode scalar value (full case mapping incl. one-to-many expansions such as ß -> SS).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CC=${HOSTCC:-gcc}
"$CC" -std=c11 -pedantic -Wall -Wextra -Werror -O2 -o "$OUT/upper_check" upper_check.c ../src/store.c ../src/engine.c -lm
"$OUT/upper_check" > "$OUT/c.txt"
node upper_ref.js > "$OUT/js.txt"
if cmp -s "$OUT/c.txt" "$OUT/js.txt"; then
    echo "seed upper-case parity: OK ($(wc -l < "$OUT/c.txt") code points)"
else
    echo "seed upper-case parity: FAILED" >&2
    diff "$OUT/c.txt" "$OUT/js.txt" | head -20 >&2
    exit 1
fi
# the generated table must be current for this node/ICU
node gen_upper.js 2>/dev/null | cmp -s - ../src/upper_table.h || echo "note: src/upper_table.h differs from this node's ICU (regenerate with gen_upper.js)"

#!/usr/bin/env bash
# Store tests: web parity fixtures from the real app.js state code (node), API round trips,
# malformed / truncated / mutated inputs. Built with ASan + UBSan.
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CC=${HOSTCC:-gcc}
FLAGS="-std=c11 -pedantic -Wall -Wextra -Werror -O1 -g -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all"
"$CC" $FLAGS -o "$OUT/test_store" test_store.c ../src/store.c ../src/engine.c -lm
node store_ref.js > "$OUT/cases.txt"
ASAN_OPTIONS=detect_leaks=1 "$OUT/test_store" "$OUT/cases.txt" "$OUT"
# the Windows build of store.c must compile cleanly too
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
    x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -Os -c ../src/store.c -o "$OUT/store_w.o"
    echo "store.c mingw build: OK"
fi

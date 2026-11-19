#!/usr/bin/env bash
# Builds native/build/IcewindDaleWeatherHelper.exe (64-bit Windows, system DLLs only).
# Requires: x86_64-w64-mingw32-gcc and x86_64-w64-mingw32-windres.
# Every native/src/*.c is compiled; engine.c additionally with -ffp-contract=off so its
# floating point matches the web engine bit for bit (never build it with -ffast-math).
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}

for i in 01 02 03 04 05 06 07 08 09 10 11 12; do
    [ -f "assets/w$i.jpg" ] || { echo "missing assets/w$i.jpg (run tools/gen_assets.py)" >&2; exit 1; }
    # 360x120 thumbnails (timeline strip, calendar cells): rebuilt when the scene changes
    if [ ! -f "assets/t$i.jpg" ] || [ "assets/w$i.jpg" -nt "assets/t$i.jpg" ]; then
        command -v convert >/dev/null || { echo "assets/t$i.jpg is stale and ImageMagick is missing" >&2; exit 1; }
        convert "assets/w$i.jpg" -filter Catrom -resize '360x120^' -gravity center -extent 360x120 \
            -strip -quality 82 -sampling-factor 4:2:0 "assets/t$i.jpg"
    fi
done

# no -fdata-sections: mingw ld then stores zero-initialised statics as raw .data (+850 KB)
CFLAGS=(-std=c11 -Os -municode -finput-charset=UTF-8
        -ffunction-sections -fno-asynchronous-unwind-tables
        -Wall -Wextra)

mkdir -p build
OBJ=$(mktemp -d build/obj.XXXXXX)          # private: parallel builds never share objects
trap 'rm -rf "$OBJ"' EXIT
"$WINDRES" --include-dir=src -O coff -o "$OBJ/app.res.o" src/app.rc

objs=()
for src in src/*.c; do
    name=$(basename "$src" .c)
    extra=()
    [ "$name" = engine ] && extra=(-ffp-contract=off)
    "$CC" "${CFLAGS[@]}" "${extra[@]}" -c "$src" -o "$OBJ/$name.o"
    objs+=("$OBJ/$name.o")
done

OUT=${OUT:-build/IcewindDaleWeatherHelper.exe}
"$CC" -s -municode -mwindows -o "${OUT%.exe}.part.exe" \
    "${objs[@]}" "$OBJ/app.res.o" \
    -Wl,--gc-sections \
    -lgdiplus -lcomctl32 -lcomdlg32 -lshlwapi -lshell32 -lole32 -luser32 -lgdi32 -lwinmm -lkernel32
mv -f "${OUT%.exe}.part.exe" "$OUT"
ls -l "$OUT"

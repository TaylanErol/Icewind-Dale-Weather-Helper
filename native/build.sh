#!/usr/bin/env bash
# Builds native/build/IcewindDaleWeatherHelper.exe (64-bit Windows, no third-party runtime).
# Requires: x86_64-w64-mingw32-gcc and x86_64-w64-mingw32-windres.
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}

for i in 01 02 03 04 05 06 07 08 09 10 11 12; do
    [ -f "assets/w$i.jpg" ] || { echo "missing assets/w$i.jpg (run tools/gen_assets.py)" >&2; exit 1; }
done

mkdir -p build
"$WINDRES" --include-dir=src -O coff -o build/app.res.o src/app.rc
"$CC" -std=c11 -Os -s -municode -mwindows \
    -finput-charset=UTF-8 \
    -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables \
    -Wall -Wextra \
    -o build/IcewindDaleWeatherHelper.exe \
    src/main.c build/app.res.o \
    -Wl,--gc-sections \
    -lgdiplus -lshlwapi -lshell32 -luser32 -lgdi32 -lkernel32
rm -f build/app.res.o
ls -l build/IcewindDaleWeatherHelper.exe

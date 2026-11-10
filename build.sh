#!/usr/bin/env bash
# MSYS2 MINGW64 kabugunda calistirin. Derleme OneDrive disinda yapilir.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cygpath -F 40)/ps-stream-build" # 40 is the user profile folder
CHIAKI="$WORK/chiaki-ng"
BUILD="$WORK/build"
EXTRA="${1:-}"

mkdir -p "$WORK"
if [ ! -d "$CHIAKI/.git" ]; then
	git -c core.autocrlf=false clone --depth 1 --branch v1.10.0 https://github.com/chiaki-ng/chiaki-ng.git "$CHIAKI"
	git -C "$CHIAKI" -c core.autocrlf=false submodule update --init --depth 1 third-party/nanopb third-party/jerasure third-party/gf-complete
	git -C "$CHIAKI" apply "$SRC/patches/chiaki-ps-stream.patch"
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCHIAKI_DIR=$CHIAKI" $EXTRA
cmake --build "$BUILD" --target ps-stream

DIST="$SRC/dist"
mkdir -p "$DIST"
cp "$BUILD/ps-stream.exe" "$DIST/PS Stream.exe"
ldd "$BUILD/ps-stream.exe" | awk '$3 ~ /mingw64/ {print $3}' | sort -u | xargs -I{} cp {} "$DIST/"
(cd "$SRC/src" && windres launcher.rc -O coff -o "$BUILD/launcher.o")
gcc -O2 -municode -mwindows -static "$SRC/src/launcher.c" "$BUILD/launcher.o" -o "$SRC/PS Stream.exe"
# The download zip carries the licenses of the libraries in dist
bash "$SRC/scripts/third_party_notices.sh" "$CHIAKI"
echo "Hazir: $SRC/PS Stream.exe"

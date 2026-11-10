#!/usr/bin/env bash
# Writes THIRD-PARTY-NOTICES.txt: every library shipped in dist, with its version, license and full
# license text, for the download zip. Run from an MSYS2 MINGW64 shell after build.sh fills dist.
set -euo pipefail
# pacman answers in the system language otherwise, and the fields are read by their English names
export LC_ALL=C

SRC="$(cd "$(dirname "$0")/.." && pwd)"
CHIAKI="${1:-$(cygpath -F 40)/ps-stream-build/chiaki-ng}"
OUT="$SRC/THIRD-PARTY-NOTICES.txt"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# pacman is slow to start on Windows, so it runs three times in all, not once per package
(cd "$SRC/dist" && ls *.dll) > "$TMP/dlls"
sed 's|^|/mingw64/bin/|' "$TMP/dlls" | xargs pacman -Qqo | sort -u > "$TMP/pkgs"
xargs pacman -Qi < "$TMP/pkgs" > "$TMP/info"
xargs pacman -Ql < "$TMP/pkgs" > "$TMP/files"

field() { awk -v p="$1" -v f="$2" '/^Name/ {n = $3} n == p && index($0, f) == 1 {sub(/^[^:]*: /, ""); print; exit}' "$TMP/info"; }

{
	echo "PS Stream - third-party notices"
	echo "==============================="
	echo
	echo "PS Stream is licensed under the GNU Affero General Public License v3 with the OpenSSL"
	echo "exception (LICENSE.txt). Its source code, including the changes made to chiaki-ng, is at"
	echo "https://github.com/Warhead500metalmonster/ps-stream"
	echo
	echo "The download also contains the libraries below. They keep their own licenses. Where a"
	echo "library is under the GPL or LGPL, its source code is available from MSYS2, which built it:"
	echo "https://github.com/msys2/MINGW-packages and https://repo.msys2.org/mingw/sources/"
	echo "If you'd rather get it from us, open an issue on the GitHub page above and we'll provide it."
	echo
	echo "Built into PS Stream.exe"
	echo "------------------------"
	echo
	echo "chiaki-ng v1.10.0 (lib), https://github.com/chiaki-ng/chiaki-ng"
	echo "  License: AGPL-3.0 with the OpenSSL exception. Changes: patches/chiaki-ps-stream.patch"
	echo "nanopb, https://github.com/nanopb/nanopb - License: Zlib"
	echo "Jerasure, https://github.com/ceph/jerasure - License: BSD-3-Clause"
	echo "GF-Complete, https://github.com/ceph/gf-complete - License: BSD-3-Clause"
	echo
	echo "Libraries in the dist folder"
	echo "----------------------------"
	echo
	while read -r p; do
		files=$(awk -v p="$p" '$1 == p && $2 ~ /^\/mingw64\/bin\/.*\.dll$/ {sub(".*/", "", $2); print $2}' "$TMP/files" | grep -Fxf "$TMP/dlls" | tr '\n' ' ' || true)
		echo "${p#mingw-w64-x86_64-} $(field "$p" Version), $(field "$p" URL)"
		echo "  License: $(field "$p" Licenses)"
		echo "  Files: $files"
	done < "$TMP/pkgs"
	echo
	echo "License texts"
	echo "============="
	awk '$2 ~ /^\/mingw64\/share\/licenses\/[^/]+\/.+[^/]$/ {print $1, $2}' "$TMP/files" | while read -r p f; do
		echo
		echo "---------- ${p#mingw-w64-x86_64-}: $(basename "$f") ----------"
		echo
		cat "$f"
	done
	# FFmpeg, x264, x265 and others ship no license text of their own in MSYS2
	for f in /mingw64/share/licenses/gcc/COPYING3:"GNU General Public License v3" /mingw64/share/licenses/binutils/COPYING:"GNU General Public License v2"; do
		if [ -f "${f%%:*}" ]; then
			echo
			echo "---------- ${f#*:} (applies to the libraries marked GPL) ----------"
			echo
			cat "${f%%:*}"
		fi
	done
	for f in "$CHIAKI"/third-party/nanopb/LICENSE.txt "$CHIAKI"/third-party/jerasure/COPYING "$CHIAKI"/third-party/gf-complete/COPYING; do
		if [ -f "$f" ]; then
			echo
			echo "---------- $(basename "$(dirname "$f")"): $(basename "$f") ----------"
			echo
			cat "$f"
		fi
	done
} | tr -d '\r' > "$OUT"
echo "Written: $OUT ($(wc -l < "$OUT") lines)"

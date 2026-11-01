#!/usr/bin/env bash
# Build and run the reconstruction tests.
#
#   native (host g++):        fast logic check
#   i686-w64-mingw32 (PE):    the real target platform, run under wine
#
# Outputs go to build/ (git-ignored).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
cd "$root"
mkdir -p build

shopt -s nullglob
tests=(src/tests/*.cpp)
if [ ${#tests[@]} -eq 0 ]; then
	echo "no tests found under src/tests" >&2
	exit 1
fi

echo "== native build (host g++) =="
for src in "${tests[@]}"; do
	name="$(basename "$src" .cpp)"
	g++ -std=c++17 -O2 -Wall -Wextra -Isrc "$src" -o "build/${name}_native"
	"./build/${name}_native"
done
g++ -std=c++17 -O2 -Wall -Wextra -Isrc src/tools/cde_recon_main.cpp \
	-o build/cde_recon_native
./build/cde_recon_native

if ! command -v i686-w64-mingw32-g++ >/dev/null; then
	echo "i686-w64-mingw32-g++ not found; skipping PE build" >&2
	exit 0
fi

echo
echo "== windows PE build (i686-w64-mingw32-g++) =="
for src in "${tests[@]}"; do
	name="$(basename "$src" .cpp)"
	i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -static -Isrc "$src" \
		-o "build/${name}.exe"
done
i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -static -Isrc \
	src/tools/cde_recon_main.cpp -o build/cde_recon.exe
file build/*.exe

if ! command -v wine >/dev/null; then
	echo "wine not found; skipping PE run" >&2
	exit 0
fi

echo
echo "== run the PE under wine =="
for exe in build/*.exe; do
	echo "-- $(basename "$exe")"
	WINEDEBUG=-all wine "$exe"
done

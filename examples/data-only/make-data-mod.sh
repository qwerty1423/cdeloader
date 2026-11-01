#!/usr/bin/env bash
# M23 example 1 of 3 — a data-only mod, built from your own game files.
#
# No DLL and no code: the loader can serve a mod's own Data/ tree over the
# game's when `[loader] redirect = 1` is set in cdeloader.ini.  A data-only mod
# is therefore just a folder of files whose paths mirror the game's:
#
#     mods/<your-mod>/Data/<path relative to the game>
#
# This script demonstrates it with the smallest safe file in the game,
# Resources/Data/Misc.txt, which starts with the game's default materials:
#
#     GameConstants Game
#     DefaultMaterial     Aluminum
#     ...
#
# It copies *your* copy of that file into a mod folder and changes the default
# material to Steel, so new modules in the ship designer start as steel.  No
# game data is redistributed: the copy is made on your machine, from your
# install, at build time.
#
#   usage: examples/data-only/make-data-mod.sh /path/to/game [/path/to/output]
#
# Then add to <game>/cdeloader.ini:
#
#     [loader]
#     redirect = 1
#
# and start the game.  cdeloader.log should show
#
#     data redirect .../Resources/Data/Misc.txt -> .../mods/example-data/Data/Misc.txt
#
# Delete the mod folder (and unset redirect) to go back to stock.
set -eu

game=${1:?usage: make-data-mod.sh <game-dir> [output-dir]}
out=${2:-$game/mods/example-data}

stock=$game/Resources/Data/Misc.txt
[ -f "$stock" ] || { echo "not a game directory (no Resources/Data/Misc.txt): $game" >&2; exit 2; }

mkdir -p "$out/Data"
cp "$stock" "$out/Data/Misc.txt"

# Change exactly one value, and only if it is still the stock line, so running
# this twice does not double-edit.
if grep -q $'^DefaultMaterial\t\tAluminum$' "$out/Data/Misc.txt"; then
    sed -i $'s/^DefaultMaterial\t\tAluminum$/DefaultMaterial\\t\\tSteel/' "$out/Data/Misc.txt"
    echo "patched DefaultMaterial Aluminum -> Steel"
elif grep -q $'^DefaultMaterial\t\tSteel$' "$out/Data/Misc.txt"; then
    echo "already patched"
else
    echo "note: DefaultMaterial line not found in the expected form; file copied unchanged" >&2
fi

cat > "$out/README.txt" <<'EOF'
Example data-only mod (built by examples/data-only/make-data-mod.sh).

Everything under Data/ mirrors the game's Resources/Data/ tree.  With
[loader] redirect = 1 in cdeloader.ini, the loader serves this copy instead of
the stock file, so the change applies without touching the game.

Currently: Data/Misc.txt with DefaultMaterial changed to Steel.
Remove this folder and set redirect = 0 to restore stock.
EOF

echo "wrote $out"
echo "add '[loader]' + 'redirect = 1' to $game/cdeloader.ini, then start the game"

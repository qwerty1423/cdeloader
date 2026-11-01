#!/bin/sh
# Launcher for the cdefw package without needing PYTHONPATH.
# Usage: scripts/cdefw.sh sdk -o cdeloader.symbols
here=$(cd "$(dirname "$0")" && pwd)
PY=$(command -v python3 || command -v python)
exec "$PY" -c 'import sys; sys.path.insert(0, sys.argv[1]); from cdefw.cli import main; raise SystemExit(main(sys.argv[2:]))' "$here" "$@"

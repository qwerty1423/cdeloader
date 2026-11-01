#!/bin/sh
# Launcher for the cdemod package without needing PYTHONPATH.
# Usage: scripts/cdemod.sh lint path/to/MyMod
here=$(cd "$(dirname "$0")" && pwd)
PY=$(command -v python3 || command -v python)
exec "$PY" -c 'import sys; sys.path.insert(0, sys.argv[1]); from cdemod.cli import main; raise SystemExit(main(sys.argv[2:]))' "$here" "$@"

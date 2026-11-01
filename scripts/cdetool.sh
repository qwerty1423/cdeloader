#!/bin/sh
# Launcher for the cdetool package without needing PYTHONPATH.
# Usage: tools/cdetool.sh info path/to/CDE.exe
here=$(cd "$(dirname "$0")" && pwd)
PY=$(command -v python3 || command -v python)
exec "$PY" -c 'import sys; sys.path.insert(0, sys.argv[1]); from cdetool.cli import main; raise SystemExit(main(sys.argv[2:]))' "$here" "$@"

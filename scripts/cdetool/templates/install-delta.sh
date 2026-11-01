#!/bin/sh
# @@NAME@@ @@VERSION@@ - legacy installer (patches CDE.exe's import table).
# Prefer install.sh (proxy mode).
here=$(cd "$(dirname "$0")" && pwd)
PY=$(command -v python3 || command -v python)
if [ -z "$PY" ]; then
    echo "Python 3 is required but was not found." >&2
    exit 1
fi
exec "$PY" "$here/installer.py" "$@"

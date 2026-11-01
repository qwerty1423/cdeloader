#!/bin/sh
# @@NAME@@ @@VERSION@@ - restore the original CDE.exe.
here=$(cd "$(dirname "$0")" && pwd)
PY=$(command -v python3 || command -v python)
exec "$PY" "$here/installer.py" --uninstall "$@"

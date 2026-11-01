#!/usr/bin/env bash
# Launch exactly one CDE instance under Wine, refusing if one is already running.
#
#   scripts/dev/run-one.sh start /path/to/game
#   scripts/dev/run-one.sh stop
#
# The previous tests leaked instances because kill patterns missed Wine's
# Z:\...\CDE.exe processes; this checks /proc directly before starting.
set -eu

find_cde() {
    # Wine's Linux process may be wine-preloader (or wineserver); the Windows
    # image name is the first cmdline argument, e.g. Z:\...\CDE.exe.
    for p in /proc/[0-9]*; do
        [ -r "$p/cmdline" ] || continue
        first=$(tr '\0' '\n' < "$p/cmdline" 2>/dev/null | head -n1 || true)
        if [ -n "$first" ]; then
            base=$(basename "${first//\\//}")
            case "${base,,}" in cde.exe) echo "${p#/proc/}"; continue;; esac
        fi
        if [ -r "$p/exe" ]; then
            exe=$(basename "$(readlink "$p/exe" 2>/dev/null || true)")
            case "${exe,,}" in cde.exe) echo "${p#/proc/}";; esac
        fi
    done
}

cmd=${1:-start}
case "$cmd" in
    start)
        dir=${2:?usage: run-one.sh start <game-dir>}
        [ -d "$dir" ] || { echo "not a directory: $dir" >&2; exit 2; }
        running=$(find_cde | sed -n 1p)
        if [ -n "$running" ]; then
            echo "refusing to start: CDE is already running (pid $running)" >&2
            exit 1
        fi
        cd "$dir"
        DISPLAY=${DISPLAY:-:0} WINEDEBUG=${WINEDEBUG:--all} \
            nohup wine CDE.exe >wine.out 2>wine.err &
        echo $! > /tmp/cde-run.pid
        echo "started CDE in $dir (pid $(cat /tmp/cde-run.pid))"
        ;;
    stop)
        pids=$(find_cde || true)
        if [ -z "$pids" ]; then echo "no CDE running"; exit 0; fi
        echo "stopping: $pids"
        # shellcheck disable=SC2086
        kill $pids 2>/dev/null || true
        rm -f /tmp/cde-run.pid
        ;;
    *)
        echo "usage: run-one.sh start <game-dir> | stop" >&2
        exit 2
        ;;
esac

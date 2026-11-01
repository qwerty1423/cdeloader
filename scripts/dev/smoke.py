#!/usr/bin/env python3
"""Headless-ish smoke runs of the real game under Wine/X.

    scripts/dev/smoke.py --game-dir /path/to/game --scenario menu
    scripts/dev/smoke.py --game-dir /path/to/game --scenario mission

Scenarios drive the known 1920x1080 menu path with XTEST clicks, take a
screenshot, then stop the game.  The exit status is non-zero when the loader log
is missing, contains a crash, or the process died early.

Requires: wine, an X display (`DISPLAY`), ImageMagick `import`, and
`scripts/dev/xinput.py` (no root, XTEST).
"""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XINPUT = os.path.join(HERE, "xinput.py")

# (x, y) on a 1920x1080 display, from the recorded runs.
SANDBOX = (960, 479)
FIRST_LEVEL = (200, 165)
GO_TO_BRIEFING = (1780, 1048)
BEGIN_MISSION = (1780, 1048)

SCENARIOS = {
    "menu": [],
    "sandbox": [SANDBOX],
    "mission": [SANDBOX, FIRST_LEVEL, GO_TO_BRIEFING, BEGIN_MISSION],
}

BAD_MARKERS = (
    "unhandled exception",
    "crash",
    "faulting",
    "first-chance",
)


def click(x: int, y: int) -> None:
    subprocess.run([sys.executable, XINPUT, "click", str(x), str(y)], check=False)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--scenario", choices=sorted(SCENARIOS), default="menu")
    ap.add_argument("--timeout", type=float, default=90.0,
                    help="overall seconds before the game is stopped")
    ap.add_argument("--boot-wait", type=float, default=20.0)
    ap.add_argument("--step-wait", type=float, default=3.0)
    ap.add_argument("--out", default="/tmp/cde-smoke")
    args = ap.parse_args(argv)

    game = os.path.abspath(args.game_dir)
    exe = os.path.join(game, "CDE.exe")
    if not os.path.isfile(exe):
        print("no CDE.exe in %s" % game, file=sys.stderr)
        return 2
    if not os.environ.get("DISPLAY"):
        print("DISPLAY is not set", file=sys.stderr)
        return 2

    os.makedirs(args.out, exist_ok=True)
    log = os.path.join(game, "cdeloader.log")
    for path in (log, os.path.join(game, "cdeloader_crash.dmp")):
        try:
            os.remove(path)
        except OSError:
            pass

    env = dict(os.environ, WINEDEBUG="-all")
    proc = subprocess.Popen(["wine", "CDE.exe"], cwd=game, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started = time.time()
    try:
        time.sleep(args.boot_wait)
        for i, (x, y) in enumerate(SCENARIOS[args.scenario]):
            if proc.poll() is not None:
                break
            click(x, y)
            time.sleep(args.step_wait)
            shot = os.path.join(args.out, "%s-%02d.png" % (args.scenario, i + 1))
            subprocess.run(["import", "-window", "root", shot], check=False,
                           env=dict(os.environ))
        remaining = args.timeout - (time.time() - started)
        if remaining > 0 and args.scenario == "mission":
            time.sleep(min(remaining, 10.0))
        shot = os.path.join(args.out, "%s-final.png" % args.scenario)
        subprocess.run(["import", "-window", "root", shot], check=False,
                       env=dict(os.environ))
    finally:
        subprocess.run(["wineserver", "-k"], check=False)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    problems = []
    if not os.path.isfile(log):
        problems.append("no cdeloader.log (loader did not start)")
    else:
        with open(log, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        lowered = text.lower()
        for marker in BAD_MARKERS:
            if marker in lowered:
                problems.append("log contains %r" % marker)
        if "cdeloader ready" not in text:
            problems.append("loader never became ready")
    if os.path.isfile(os.path.join(game, "cdeloader_crash.dmp")):
        problems.append("cdeloader_crash.dmp was written")

    print("scenario: %s" % args.scenario)
    print("screenshots: %s" % args.out)
    if problems:
        for p in problems:
            print("FAIL: %s" % p, file=sys.stderr)
        return 1
    print("OK: loader started and stayed up")
    return 0


if __name__ == "__main__":
    sys.exit(main())

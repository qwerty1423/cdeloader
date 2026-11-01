#!/usr/bin/env python3
"""CDE Mod Framework installer.

    python3 install.py /path/to/game
    python3 install.py '<game folder>'        (a Wine path, resolved through WINEPREFIX)

Safe to run again over an existing install: plugin settings, disabled plugins and
anything you added yourself are left alone.  Requires Python 3.7 or newer and
nothing else.
"""

import argparse
import hashlib
import os
import re
import shutil
import sys

WINE_DRIVE = re.compile(r"^([A-Za-z]):[\\/](.*)$")

# Builds this framework was tested against.  An unknown hash is reported, not
# refused: a different game build may still work, and the loader says so in its log.
KNOWN_EXE = {
    "04fa7f2218211cbcc8f041bcd81077cc4b0c7f5466603d7b26dae9c9e8bf3db2":
        "stock 1.2.1.3",
    "26c70b79fcac69d61ffb23dfe5c2fc4c8611a2a4924a53b4fe4a46f863292465":
        "stock plus the community patch",
}

# The runtime the game ships with.  Other Visual C++ redistributable versions are
# legitimate; the forwarder passes whatever is renamed next to it.
KNOWN_RUNTIME = {
    "1b372f064eacb455a0351863706e6326ca31b08e779a70de5de986b5be8069a1":
        "the Visual C++ runtime shipped with the game",
}

# The forwarder this package installs.  Filled in when the zip is built, so an
# uninstall only deletes vcruntime140.dll when it really is ours.
FORWARDER_SHA256 = "@@FORWARDER_SHA@@"

LEFTOVER_FILES = (
    "install-delta.cmd", "install-proxy.cmd", "install.cmd", "install.sh",
    "CDE-framework.delta.json", "cdeloader.delta.json",
    "cdeloader-compat-complete.delta.json", "cdeloader-compat-fixes-only.delta.json",
    "SOURCE.txt", "README.txt",
)
LEFTOVER_DIRS = ("proxy", "examples", "sdk", "source")
LEFTOVER_DIR_MARKERS = ("README.md", "plugin.dll", "vcruntime140.dll", "cdeloader.dll")


def fail(message):
    print("install: %s" % message, file=sys.stderr)
    raise SystemExit(1)


def resolve_game(path):
    """Turn the given path into a real directory, understanding Wine paths."""
    match = WINE_DRIVE.match(path)
    if match:
        drive, rest = match.group(1).lower(), match.group(2).replace("\\", "/")
        prefix = os.environ.get("WINEPREFIX") or os.path.expanduser("~/.wine")
        path = os.path.join(prefix, "drive_" + drive, rest)
        print("Wine path resolved to %s" % path)
    path = os.path.abspath(os.path.expanduser(path))
    if not os.path.isdir(path):
        fail("no such folder: %s" % path)
    return path


def read_mod_hashes(src):
    """The plugin hashes shipped in MODS.sha256, keyed by archive path."""
    path = os.path.join(src, "MODS.sha256")
    hashes = {}
    if not os.path.isfile(path):
        return hashes
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            digest, _, name = line.partition("  ")
            if name:
                hashes[name] = digest
    return hashes


def read_installed_hashes(game):
    """The hashes the last run recorded, keyed by plugin folder."""
    path = os.path.join(game, "mods", ".coade-hashes")
    hashes = {}
    if not os.path.isfile(path):
        return hashes
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            digest, _, name = line.partition("  ")
            folder = name.split("/")[0] if name else ""
            if folder and digest:
                hashes[folder] = digest
    return hashes


def game_processes(game):
    """PIDs that have a file from this folder open or mapped.

    Command lines are unreliable for a Wine program: the process is a wine loader and
    the game path may appear in Windows form.  What cannot be faked is the mapping, so
    ask the kernel which process has CDE.exe or one of the loader files open.
    """
    found = []
    if not os.path.isdir("/proc"):
        return found
    target = os.path.realpath(game) + os.sep
    names = ("cde.exe", "cdeloader.dll", "vcruntime140.dll", "cdeloader.symbols")
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        for source in ("maps", "fd"):
            base = "/proc/%s/%s" % (entry, source)
            try:
                if source == "maps":
                    with open(base, "rb") as fh:
                        text = fh.read().decode("utf-8", "replace")
                    candidates = [line.rsplit(" ", 1)[-1] for line in text.splitlines()]
                else:
                    candidates = [os.readlink(os.path.join(base, fd))
                                  for fd in os.listdir(base)]
            except OSError:
                continue
            for path in candidates:
                if not path.startswith(target):
                    continue
                if any(path.lower().endswith(name) for name in names):
                    found.append(pid)
                    break
            if pid in found:
                break
    return found


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def report_hash(label, path, known):
    """Print one hash line; unknown builds are flagged but not refused."""
    if not os.path.isfile(path):
        print("  %-14s missing" % label)
        return
    digest = sha256_of(path)
    note = known.get(digest)
    if note:
        print("  %-14s %s  (%s)" % (label, digest, note))
    else:
        print("  %-14s %s  (not a build this framework was tested with; "
              "see the readme)" % (label, digest))


def install(src, game, dry_run=False, keep_settings=False):
    steps = []

    if os.path.exists(os.path.join(game, "vcruntime140_orig.dll")):
        steps.append(("keep", "vcruntime140_orig.dll (the runtime is already renamed)"))
    else:
        steps.append(("rename", "vcruntime140.dll -> vcruntime140_orig.dll"))

    for name in ("vcruntime140.dll", "cdeloader.dll", "cdeloader.symbols"):
        if not os.path.isfile(os.path.join(src, name)):
            fail("this zip is missing %s" % name)
        steps.append(("copy", name))

    keep_ini = os.path.exists(os.path.join(game, "cdeloader.ini"))
    if keep_ini:
        steps.append(("keep", "cdeloader.ini"))
    else:
        steps.append(("copy", "cdeloader.ini"))

    mods_src = os.path.join(src, "mods")
    def dlls_in(folder):
        return sorted(name for name in os.listdir(folder)
                      if name.lower().endswith(".dll"))

    plugins = sorted(
        name for name in os.listdir(mods_src)
        if name != "config" and os.path.isdir(os.path.join(mods_src, name))
        and dlls_in(os.path.join(mods_src, name))
    ) if os.path.isdir(mods_src) else []

    for name in plugins:
        if os.path.isdir(os.path.join(game, "mods", "disabled", name)):
            steps.append(("plugin", name, "disabled"))
        else:
            steps.append(("plugin", name, "enabled"))

    config_src = os.path.join(mods_src, "config")
    new_inis = []
    if os.path.isdir(config_src):
        for name in sorted(os.listdir(config_src)):
            if not name.endswith(".ini"):
                continue
            if not os.path.exists(os.path.join(game, "mods", "config", name)):
                new_inis.append(name)
    steps.extend(("ini", name) for name in new_inis)

    manifest = os.path.join(game, "mods", ".coade-installed")
    previous = []
    if os.path.isfile(manifest):
        with open(manifest, encoding="utf-8") as fh:
            previous = [line.strip() for line in fh if line.strip()]
    retired = [name for name in previous if name not in plugins]
    steps.extend(("retire", name) for name in retired)

    # What the plugin files should hash to, so a bad copy is caught now rather than
    # as a mysterious crash later.
    expected = read_mod_hashes(src)
    checked = {}
    for name in plugins:
        for dll in dlls_in(os.path.join(mods_src, name)):
            key = "mods/%s/%s" % (name, dll)
            if key in expected:
                checked["%s/%s" % (name, dll)] = expected[key]

    # A new plugin build can add or rename settings, so an old ini may configure it
    # wrongly.  When the file itself changed, the ini goes back to the packaged
    # default and the user's copy is kept next to it as .bak.
    installed_before = {}
    for key, digest in read_installed_hashes(game).items():
        installed_before[key.split("/")[0]] = digest
    ini_for_folder = {}
    if os.path.isdir(config_src):
        for name in sorted(os.listdir(config_src)):
            if not name.endswith(".ini"):
                continue
            stem = name[:-len(".ini")]
            for folder in plugins:
                if stem.endswith("." + folder) or stem == folder:
                    ini_for_folder[folder] = name
    new_by_folder = {}
    for key, digest in checked.items():
        new_by_folder[key.split("/")[0]] = digest

    resets = []
    if not keep_settings:
        for name in plugins:
            new_hash = new_by_folder.get(name)
            old_hash = installed_before.get(name)
            if not new_hash or not old_hash or new_hash == old_hash:
                continue
            target = os.path.join(game, "mods", "config",
                                  ini_for_folder.get(name, "org.cdere.%s.ini" % name))
            if os.path.isfile(target):
                resets.append(name)
                if not dry_run:
                    # keep what the user had, and start again from the packaged ini
                    shutil.copy2(target, target + ".bak")
                    packaged = os.path.join(config_src,
                                            ini_for_folder.get(name, ""))
                    if ini_for_folder.get(name) and os.path.isfile(packaged):
                        shutil.copy2(packaged, target)
                    else:
                        os.remove(target)
    if resets:
        steps.extend(("reset", name) for name in resets)

    leftovers = []
    for name in LEFTOVER_FILES:
        if os.path.isfile(os.path.join(game, name)):
            leftovers.append(name)
    for name in LEFTOVER_DIRS:
        folder = os.path.join(game, name)
        if os.path.isdir(folder) and any(
                os.path.isfile(os.path.join(folder, marker))
                for marker in LEFTOVER_DIR_MARKERS):
            leftovers.append(name + "/")
    steps.extend(("remove", name) for name in leftovers)

    for step in steps:
        kind = step[0]
        if kind == "keep":
            print("  keeping %s" % step[1])
            continue
        if kind == "rename":
            print("  renaming %s" % step[1])
            if not dry_run:
                os.replace(os.path.join(game, "vcruntime140.dll"),
                           os.path.join(game, "vcruntime140_orig.dll"))
        elif kind == "copy":
            print("  copying %s" % step[1])
            if not dry_run:
                shutil.copy2(os.path.join(src, step[1]), os.path.join(game, step[1]))
        elif kind == "plugin":
            name, state = step[1], step[2]
            target = os.path.join(game, "mods", name)
            if state == "disabled":
                target = os.path.join(game, "mods", "disabled", name)
                print("  updating %s (disabled, stays disabled)" % name)
            else:
                print("  installing %s" % name)
            if not dry_run:
                os.makedirs(target, exist_ok=True)
                for dll in dlls_in(os.path.join(mods_src, name)):
                    shutil.copy2(os.path.join(mods_src, name, dll),
                                 os.path.join(target, dll))
        elif kind == "ini":
            print("  adding settings %s" % step[1])
            if not dry_run:
                os.makedirs(os.path.join(game, "mods", "config"), exist_ok=True)
                shutil.copy2(os.path.join(config_src, step[1]),
                             os.path.join(game, "mods", "config", step[1]))
        elif kind == "retire":
            print("  removing %s (no longer shipped)" % step[1])
            if not dry_run:
                for base in ("mods", os.path.join("mods", "disabled")):
                    folder = os.path.join(game, base, step[1])
                    if os.path.isdir(folder):
                        shutil.rmtree(folder)
        elif kind == "reset":
            print("  resetting settings for %s (the plugin changed, kept as .bak)"
                  % step[1])
        elif kind == "remove":
            print("  removing %s (left by an older version)" % step[1].rstrip("/"))
            if not dry_run:
                target = os.path.join(game, step[1].rstrip("/"))
                if os.path.isdir(target):
                    shutil.rmtree(target)
                elif os.path.isfile(target):
                    os.remove(target)

    if not dry_run:
        os.makedirs(os.path.join(game, "mods", "config"), exist_ok=True)
        with open(manifest, "w", encoding="utf-8") as fh:
            fh.write("\n".join(plugins) + ("\n" if plugins else ""))

    verified, mismatched = [], []
    if not dry_run and checked:
        for name, digest in checked.items():
            for base in ("mods", os.path.join("mods", "disabled")):
                target = os.path.join(game, base, *name.split("/"))
                if not os.path.isfile(target):
                    continue
                actual = sha256_of(target)
                (verified if actual == digest else mismatched).append(
                    (os.path.join(base, *name.split("/")), actual, digest))
                break
        # remember the hashes for the game folder, so the loader can verify each
        # start and log a warning when a plugin has been replaced
        lines = ["%s  %s" % (digest, name) for name, digest in sorted(checked.items())]
        with open(os.path.join(game, "mods", ".coade-hashes"), "w",
                  encoding="utf-8") as fh:
            fh.write("\n".join(lines) + ("\n" if lines else ""))

    print("")
    print("%s %d plugin(s) into %s" % ("Would install" if dry_run else "Installed",
                                       len(plugins), game))
    if verified:
        print("Verified %d plugin file(s) against the hashes in this zip"
              % len(verified))
    for name, actual, digest in mismatched:
        print("  WARNING: %s does not match the hash in this zip" % name)
        print("           copied %s" % actual)
        print("           expected %s" % digest)
    if resets:
        print("Reset settings for %d plugin(s) that changed; the old files are "
              "kept as .bak" % len(resets))
    if retired:
        print("Removed %d plugin(s) that are no longer shipped" % len(retired))
    if any(step[2] == "disabled" for step in steps if step[0] == "plugin"):
        print("Plugins in mods/disabled were updated in place and stay disabled")
    print("Start the game and press F2.")


def uninstall(game, dry_run=False):
    """Remove what this framework installed, and nothing else."""
    removed, kept = [], []
    forwarder = os.path.join(game, "vcruntime140.dll")
    original = os.path.join(game, "vcruntime140_orig.dll")

    manifest = os.path.join(game, "mods", ".coade-installed")
    plugins = []
    if os.path.isfile(manifest):
        with open(manifest, encoding="utf-8") as fh:
            plugins = [line.strip() for line in fh if line.strip()]

    for name in plugins:
        for base in ("mods", os.path.join("mods", "disabled")):
            folder = os.path.join(game, base, name)
            if os.path.isdir(folder):
                removed.append(folder)
                if not dry_run:
                    shutil.rmtree(folder)
        config = os.path.join(game, "mods", "config")
        if os.path.isdir(config):
            for entry in sorted(os.listdir(config)):
                stem = entry[:-len(".ini.bak")] if entry.endswith(".ini.bak") else (
                    entry[:-len(".ini")] if entry.endswith(".ini") else None)
                if stem is None:
                    continue
                if stem.endswith("." + name) or stem == name:
                    path = os.path.join(config, entry)
                    removed.append(path)
                    if not dry_run:
                        os.remove(path)

    for name in (".coade-installed", ".coade-hashes", ".coade-hashes.tmp"):
        path = os.path.join(game, "mods", name)
        if os.path.isfile(path):
            removed.append(path)
            if not dry_run:
                os.remove(path)

    for name in ("cdeloader.dll", "cdeloader.symbols", "cdeloader.ini"):
        path = os.path.join(game, name)
        if os.path.isfile(path):
            removed.append(path)
            if not dry_run:
                os.remove(path)

    # The forwarder is ours only when it hashes to the shipped one; the game's own
    # runtime must be put back, and anything unexpected in that name is left alone.
    if os.path.isfile(forwarder):
        ours = FORWARDER_SHA256 and sha256_of(forwarder) == FORWARDER_SHA256
        if ours:
            removed.append(forwarder)
            if not dry_run:
                os.remove(forwarder)
        else:
            kept.append("%s (not the forwarder this package installs, left alone)"
                        % forwarder)
    if os.path.isfile(original):
        if os.path.isfile(forwarder) and not any(
                p == forwarder for p in removed):
            kept.append(original)
        else:
            removed.append("%s -> vcruntime140.dll" % original)
            if not dry_run:
                os.replace(original, forwarder)
    elif not os.path.isfile(forwarder):
        kept.append("no vcruntime140 runtime in the folder; the game will not start "
                    "until one is restored")

    # Drop mods/ and mods/config/ only when nothing of the user's is left in them.
    for path in (os.path.join(game, "mods", "config"), os.path.join(game, "mods")):
        if os.path.isdir(path):
            try:
                empty = not os.listdir(path)
            except OSError:
                empty = False
            if empty:
                removed.append(path + "/")
                if not dry_run:
                    os.rmdir(path)

    print("")
    print("%s %d item(s) from %s" % ("Would remove" if dry_run else "Removed",
                                     len(removed), game))
    for item in removed:
        print("  %s" % item)
    for item in kept:
        print("  kept %s" % item)
    print("")
    print("Anything you added to mods/ yourself was left in place.")
    print("cdeloader.log was left behind; delete it if you do not want it.")


def main(argv):
    parser = argparse.ArgumentParser(
        description="Install the CDE mod framework and its plugins into the game "
                    "folder. Safe to run again to update.")
    parser.add_argument("game", nargs="?",
                        help="the game folder, the one containing CDE.exe (required)")
    parser.add_argument("--game", dest="game_flag", metavar="PATH",
                        help="same as the positional argument")
    parser.add_argument("--dry-run", action="store_true",
                        help="print what would happen and change nothing")
    parser.add_argument("--uninstall", action="store_true",
                        help="remove the framework and the plugins it installed")
    parser.add_argument("--force", action="store_true",
                        help="install even when the game appears to be running")
    parser.add_argument("--keep-settings", action="store_true",
                        help="never reset a plugin's ini, even when the plugin "
                             "itself changed")
    args = parser.parse_args(argv)

    game = args.game_flag or args.game
    if not game:
        # No prompting and no guessing: the game folder has to be given.
        parser.error("give the game folder, the one with CDE.exe in it "
                     "(for example: python3 install.py ~/Games/CDE)")

    game = resolve_game(game)
    if not os.path.isfile(os.path.join(game, "CDE.exe")):
        fail("no CDE.exe in %s" % game)
    if args.uninstall:
        print("Uninstalling from %s" % game)
        uninstall(game, dry_run=args.dry_run)
        return 0
    if not (os.path.isfile(os.path.join(game, "vcruntime140.dll"))
            or os.path.isfile(os.path.join(game, "vcruntime140_orig.dll"))):
        fail("no vcruntime140.dll in %s; that does not look like the game folder"
             % game)

    src = os.path.dirname(os.path.abspath(__file__))
    running = game_processes(game)
    if running and not args.force:
        print("The game appears to be running (pid %s)." %
              ", ".join(str(p) for p in running))
        print("Under Wine the files can usually still be replaced, but the running "
              "process keeps the old code and only the next launch uses the new one. "
              "Close the game and run this again, or add --force to go ahead.")
        return 1
    print("Installing into %s" % game)
    exe = os.path.join(game, "CDE.exe")
    forwarder = os.path.join(game, "vcruntime140.dll")
    original = os.path.join(game, "vcruntime140_orig.dll")
    report_hash("CDE.exe", exe, KNOWN_EXE)

    # vcruntime140.dll is our forwarder once the framework is installed, so the file
    # worth reporting is the game's own runtime, kept as vcruntime140_orig.dll.
    expected_forwarder = (FORWARDER_SHA256 if not FORWARDER_SHA256.startswith("@@")
                          else "")
    if os.path.isfile(forwarder):
        digest = sha256_of(forwarder)
        if expected_forwarder and digest == expected_forwarder:
            print("  %-14s %s  (this package's forwarder; already installed)"
                  % ("vcruntime140.dll", digest))
            runtime = original
        else:
            runtime = forwarder
    else:
        runtime = original
    report_hash("runtime", runtime, KNOWN_RUNTIME)
    print("")
    install(src, game, dry_run=args.dry_run,
            keep_settings=args.keep_settings)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

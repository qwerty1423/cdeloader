"""The packaged installer must be safe to run over an existing install."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INSTALLER = os.path.join(ROOT, "scripts", "cdefw", "templates", "install.py")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


def read(path):
    with open(path) as fh:
        return fh.read()


class InstallerScriptTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cde-installer-test-")
        # a stand-in for the extracted package
        self.src = os.path.join(self.tmp, "package")
        write(os.path.join(self.src, "vcruntime140.dll"), b"forwarder")
        write(os.path.join(self.src, "cdeloader.dll"), b"loader" * 400)
        write(os.path.join(self.src, "cdeloader.symbols"), b"symbols" * 100)
        write(os.path.join(self.src, "cdeloader.ini"), b"[loader]\nconsole = 1\n")
        for name in ("node-editor", "laser-cap"):
            write(os.path.join(self.src, "mods", name, "%s.dll" % name), b"dll" * 400)
            write(os.path.join(self.src, "mods", "config", "org.cdere.%s.ini" % name),
                  b"[mod]\nenabled = 1\n")
        shutil.copy2(INSTALLER, os.path.join(self.src, "install.py"))
        # the hash manifest the package ships
        lines = []
        import hashlib
        for name in ("node-editor", "laser-cap"):
            digest = hashlib.sha256(b"dll" * 400).hexdigest()
            lines.append("%s  mods/%s/%s.dll" % (digest, name, name))
        write(os.path.join(self.src, "MODS.sha256"), ("\n".join(lines) + "\n").encode())

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _game(self, name="game"):
        game = os.path.join(self.tmp, name)
        write(os.path.join(game, "CDE.exe"), b"exe")
        write(os.path.join(game, "vcruntime140.dll"), b"runtime")
        os.makedirs(os.path.join(game, "mods", "config"))
        return game

    def _run(self, game, *extra):
        return subprocess.run([sys.executable, os.path.join(self.src, "install.py"),
                               *extra, game],
                              capture_output=True, text=True, timeout=120)

    def test_fresh_install(self):
        game = self._game()
        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertTrue(os.path.isfile(os.path.join(game, "vcruntime140_orig.dll")))
        for name in ("cdeloader.dll", "cdeloader.symbols", "cdeloader.ini"):
            self.assertTrue(os.path.isfile(os.path.join(game, name)), name)
        self.assertTrue(os.path.isfile(
            os.path.join(game, "mods", "node-editor", "node-editor.dll")))
        self.assertTrue(os.path.isfile(
            os.path.join(game, "mods", "config", "org.cdere.node-editor.ini")))
        self.assertEqual(read(os.path.join(game, "mods", ".coade-installed")).split(),
                         ["laser-cap", "node-editor"])

    def test_refuses_a_folder_without_the_game(self):
        game = os.path.join(self.tmp, "empty")
        os.makedirs(game)
        out = self._run(game)
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("CDE.exe", out.stderr)

    def test_keeps_settings_and_disabled_plugins(self):
        game = self._game()
        config = os.path.join(game, "mods", "config", "org.cdere.node-editor.ini")
        write(config, b"[mod]\nenabled = 0\nstep_kms = 0.05\n")
        write(os.path.join(game, "cdeloader.ini"), b"[loader]\nlog_level = 3\n")
        disabled = os.path.join(game, "mods", "disabled", "laser-cap")
        write(os.path.join(disabled, "laser-cap.dll"), b"old")

        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)

        self.assertEqual(read(config), "[mod]\nenabled = 0\nstep_kms = 0.05\n")
        self.assertEqual(read(os.path.join(game, "cdeloader.ini")),
                         "[loader]\nlog_level = 3\n")
        self.assertGreater(os.path.getsize(os.path.join(disabled, "laser-cap.dll")), 1000)
        self.assertFalse(os.path.isdir(os.path.join(game, "mods", "laser-cap")))

    def test_removes_plugins_it_installed_before(self):
        game = self._game()
        self.assertEqual(self._run(game).returncode, 0)
        stale = os.path.join(game, "mods", "retired-plugin")
        write(os.path.join(stale, "retired-plugin.dll"), b"x")
        with open(os.path.join(game, "mods", ".coade-installed"), "a") as fh:
            fh.write("retired-plugin\n")
        mine = os.path.join(game, "mods", "my-own-plugin")
        write(os.path.join(mine, "my-own-plugin.dll"), b"mine")

        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertFalse(os.path.isdir(stale), "a retired plugin was left behind")
        self.assertTrue(os.path.isdir(mine),
                        "a folder the installer never created was removed")

    def test_removes_leftovers_from_old_layouts(self):
        game = self._game()
        for name in ("install-delta.cmd", "CDE-framework.delta.json", "SOURCE.txt"):
            write(os.path.join(game, name), b"old")
        write(os.path.join(game, "proxy", "vcruntime140.dll"), b"x")

        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        for name in ("install-delta.cmd", "CDE-framework.delta.json", "SOURCE.txt"):
            self.assertFalse(os.path.exists(os.path.join(game, name)), name)
        self.assertFalse(os.path.isdir(os.path.join(game, "proxy")))

    def test_reports_hashes_and_flags_unknown_builds(self):
        game = self._game()
        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("CDE.exe", out.stdout)
        self.assertIn("not a build this framework was tested with", out.stdout)

    def test_recognises_a_supported_exe(self):
        game = self._game()
        # a stand-in for the stock build: the installer must recognise the recorded
        # hash rather than complain about it
        installer = os.path.join(self.src, "install.py")
        source = read(installer)
        stock = "04fa7f2218211cbcc8f041bcd81077cc4b0c7f5466603d7b26dae9c9e8bf3db2"
        write(os.path.join(game, "CDE.exe"), b"exe")
        import hashlib
        local = hashlib.sha256(b"exe").hexdigest()
        patched = source.replace(stock, local)
        write(installer, patched.encode())
        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("stock 1.2.1.3", out.stdout)

    def test_recognises_the_community_patched_exe(self):
        game = self._game()
        installer = os.path.join(self.src, "install.py")
        source = read(installer)
        patched_hash = ("26c70b79fcac69d61ffb23dfe5c2fc4c8611a2a4924a53b4fe4a46f863292465")
        write(os.path.join(game, "CDE.exe"), b"patched exe")
        import hashlib
        local = hashlib.sha256(b"patched exe").hexdigest()
        write(installer, source.replace(patched_hash, local).encode())
        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("stock plus the community patch", out.stdout)

    def test_verifies_plugin_hashes_and_records_them(self):
        game = self._game()
        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("Verified 2 plugin file", out.stdout)
        recorded = read(os.path.join(game, "mods", ".coade-hashes"))
        self.assertIn("node-editor/node-editor.dll", recorded)

    def test_reports_a_plugin_that_does_not_match(self):
        game = self._game()
        write(os.path.join(self.src, "mods", "laser-cap", "laser-cap.dll"), b"tampered")
        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("does not match the hash", out.stdout)
        self.assertIn("laser-cap/laser-cap.dll", out.stdout)

    def _change_plugin(self, folder):
        """Rebuild one plugin's file and update the manifest, as an update would."""
        import hashlib
        dll = os.path.join(self.src, "mods", folder, "%s.dll" % folder)
        write(dll, b"new build of " + folder.encode())
        digest = hashlib.sha256(open(dll, "rb").read()).hexdigest()
        manifest = os.path.join(self.src, "MODS.sha256")
        lines = [line for line in read(manifest).splitlines() if folder not in line]
        lines.append("%s  mods/%s/%s.dll" % (digest, folder, folder))
        write(manifest, ("\n".join(lines) + "\n").encode())

    def test_resets_settings_when_a_plugin_changes(self):
        game = self._game()
        self.assertEqual(self._run(game).returncode, 0)
        ini = os.path.join(game, "mods", "config", "org.cdere.laser-cap.ini")
        write(ini, b"[mod]\nenabled = 1\nscale = 9\n")

        self._change_plugin("laser-cap")
        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("resetting settings for laser-cap", out.stdout)
        # the packaged default replaces it, and the old file is kept
        self.assertNotIn("scale = 9", read(ini))
        self.assertIn("enabled = 1", read(ini))
        self.assertEqual(read(ini + ".bak"), "[mod]\nenabled = 1\nscale = 9\n")

    def test_keeps_settings_when_nothing_changed(self):
        game = self._game()
        self.assertEqual(self._run(game).returncode, 0)
        ini = os.path.join(game, "mods", "config", "org.cdere.laser-cap.ini")
        write(ini, b"[mod]\nscale = 9\n")
        out = self._run(game)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertNotIn("resetting settings", out.stdout)
        self.assertEqual(read(ini), "[mod]\nscale = 9\n")

    def test_keep_settings_flag_suppresses_the_reset(self):
        game = self._game()
        self.assertEqual(self._run(game).returncode, 0)
        ini = os.path.join(game, "mods", "config", "org.cdere.laser-cap.ini")
        write(ini, b"[mod]\nscale = 5\n")
        self._change_plugin("laser-cap")
        out = self._run(game, "--keep-settings")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertNotIn("resetting settings", out.stdout)
        self.assertEqual(read(ini), "[mod]\nscale = 5\n")

    def test_reports_the_game_runtime_when_the_forwarder_is_installed(self):
        game = self._game()
        # the template carries a placeholder; the package fills in the real hash
        installer = os.path.join(self.src, "install.py")
        import hashlib
        forwarder = os.path.join(self.src, "vcruntime140.dll")
        digest = hashlib.sha256(open(forwarder, "rb").read()).hexdigest()
        write(installer, read(installer).replace("@@FORWARDER_SHA@@", digest).encode())

        first = self._run(game)
        self.assertEqual(first.returncode, 0, first.stderr)
        runtime_hash = hashlib.sha256(b"runtime").hexdigest()
        self.assertIn("runtime        %s" % runtime_hash, first.stdout)

        second = self._run(game)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("this package's forwarder; already installed", second.stdout)
        # the forwarder line is not mistaken for the game's runtime
        self.assertIn("runtime        %s" % runtime_hash, second.stdout)

    def test_uninstall_removes_only_its_own_files(self):
        game = self._game()
        mine = os.path.join(game, "mods", "my-own-plugin")
        write(os.path.join(mine, "my-own-plugin.dll"), b"mine")
        self.assertEqual(self._run(game).returncode, 0)
        runtime = os.path.join(game, "vcruntime140_orig.dll")
        self.assertTrue(os.path.isfile(runtime))

        out = self._run(game, "--uninstall")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertFalse(os.path.exists(os.path.join(game, "cdeloader.dll")))
        self.assertFalse(os.path.exists(os.path.join(game, "cdeloader.symbols")))
        self.assertFalse(os.path.exists(os.path.join(game, "cdeloader.ini")))
        self.assertFalse(os.path.isdir(os.path.join(game, "mods", "node-editor")))
        self.assertFalse(os.path.exists(os.path.join(game, "mods", ".coade-installed")))
        # the game's runtime is back and the user's own plugin survived
        self.assertTrue(os.path.isfile(os.path.join(game, "vcruntime140.dll")))
        self.assertTrue(os.path.isdir(mine))
        self.assertTrue(os.path.isfile(os.path.join(mine, "my-own-plugin.dll")))

    def test_uninstall_leaves_a_foreign_runtime_alone(self):
        game = self._game()
        self.assertEqual(self._run(game).returncode, 0)
        # someone replaced the forwarder with their own file
        write(os.path.join(game, "vcruntime140.dll"), b"not our forwarder")
        out = self._run(game, "--uninstall")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("left alone", out.stdout)
        self.assertEqual(read(os.path.join(game, "vcruntime140.dll")),
                         "not our forwarder")

    def test_uninstall_dry_run_changes_nothing(self):
        game = self._game()
        self.assertEqual(self._run(game).returncode, 0)
        before = sorted(os.listdir(game))
        out = self._run(game, "--uninstall", "--dry-run")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("Would remove", out.stdout)
        self.assertEqual(sorted(os.listdir(game)), before)

    def test_refuses_while_the_game_is_running(self):
        game = self._game()
        self.assertEqual(self._run(game).returncode, 0)

        # simulate a process holding this game's CDE.exe mapped
        fake = os.path.join(self.tmp, "procfake")
        pid_dir = os.path.join(fake, "4242")
        os.makedirs(pid_dir)
        exe = os.path.join(game, "CDE.exe")
        write(os.path.join(pid_dir, "maps"),
              ("7f00-7f01 r-xp 00000000 00:00 0 %s\n" % exe).encode())
        os.makedirs(os.path.join(pid_dir, "fd"))
        script = os.path.join(self.src, "install.py")
        text = read(script)
        text = text.replace('"/proc"', '"%s"' % fake)
        text = text.replace('"/proc/%s/%s"', '"%s/%%s/%%s"' % fake)
        text = text.replace('"/proc/%s/cmdline"', '"%s/%%s/cmdline"' % fake)
        write(script, text.encode())

        out = self._run(game)
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("appears to be running", out.stdout)
        self.assertEqual(self._run(game, "--force").returncode, 0)

    def test_requires_a_path(self):
        out = subprocess.run([sys.executable, os.path.join(self.src, "install.py")],
                             capture_output=True, text=True, timeout=60,
                             stdin=subprocess.DEVNULL)
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("give the game folder", out.stderr)

    def test_dry_run_changes_nothing(self):
        game = self._game()
        before = sorted(os.listdir(game))
        out = self._run(game, "--dry-run")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(sorted(os.listdir(game)), before)
        self.assertIn("Would install", out.stdout)

    def test_wine_path_is_resolved(self):
        prefix = os.path.join(self.tmp, "prefix")
        game = self._game(os.path.join("prefix", "drive_c", "CoaDE"))
        env = dict(os.environ, WINEPREFIX=prefix)
        out = subprocess.run(
            [sys.executable, os.path.join(self.src, "install.py"), "<game folder>"],
            capture_output=True, text=True, timeout=120, env=env)
        if not os.path.isdir(os.path.join(prefix, "drive_c")):
            self.assertNotEqual(out.returncode, 0)
            return
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertTrue(os.path.isfile(os.path.join(game, "cdeloader.dll")))


if __name__ == "__main__":
    unittest.main()

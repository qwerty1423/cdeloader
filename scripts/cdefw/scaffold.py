"""Scaffold a new plugin project for the CDE mod loader."""

from __future__ import annotations

import os
import re

_PLUGIN_CPP = """\
// {name} — a CDE mod-loader plugin.
//
// Build (32-bit PE): see build.sh in this folder.  Install: copy the built
// plugin.dll to <game>/mods/{dir}/plugin.dll
//
// The SDK's entry macro needs the user functions declared before the header is
// included (the header's inline trampolines call them), so forward-declare
// first, set the macros, include, then define.
#include <cstdint>

static void my_init(void);
static void my_frame(void);
static void my_shutdown(void);

#define CDE_USER_INIT()     my_init()
#define CDE_USER_FRAME()    my_frame()
#define CDE_USER_SHUTDOWN() my_shutdown()

#include "cdemod.hpp"

static void my_init(void) {{
    CDE_INFO("{name} v1.0.0 starting");
    // Example: resolve a PDB name and hook it.
    //   uintptr_t fn = cde::Resolve("SomeClass::SomeMethod");
    //   if (fn) cde::Hook("SomeMethod", (void*)fn, (void*)&my_detour);
    // Example: add a panel to the in-game menu (F10); needs the loader import
    // library — see build.sh.
    //   cde::UI("My Plugin", my_panel);
}}

static void my_frame(void) {{
    // Called every frame (approx. 60 Hz) by the loader.
}}

static void my_shutdown(void) {{
    CDE_INFO("{name} shutting down");
}}

CDE_MOD("{modid}", "{name}", "1.0.0", "{author}")
"""

_BUILD_SH = """\
#!/usr/bin/env bash
# Build this plugin as a 32-bit Windows PE.  SDK points at the folder holding
# cdemod.hpp / cdemod_api.h (this repo: src/modloader).  LOADER_IMPLIB is the
# import library for cdeloader.dll; it is only needed if the plugin uses the
# cde_ui_* overlay API (cde::UI / cde_ui_panel).
set -euo pipefail
SDK="${SDK:-../../src/modloader}"
LOADER_IMPLIB="${LOADER_IMPLIB:-../../build/modloader/libcdeloader.a}"
CXX="${CXX:-i686-w64-mingw32-g++}"
extra=()
if [ -f "$LOADER_IMPLIB" ]; then extra=("$LOADER_IMPLIB"); fi
"$CXX" -std=c++17 -O2 -Wall -Wextra -shared -fno-exceptions -fno-rtti \\
    -static -static-libgcc -static-libstdc++ -I"$SDK" \\
    -o plugin.dll plugin.cpp "${extra[@]}"
echo "built plugin.dll"
"""


def _slug(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", name).strip("-") or "plugin"


_DATA_README = """\
# {name}

A CDE **data mod** (id `{modid}`). It is installed as a folder next to the
game's `CDE.exe`:

```
mods/{slug}/
  Data/      <- files here shadow the game's own, by relative path
  mod.ini
```

Copy a game data file in at the same relative path the game uses, e.g.
`Data/Resources/Data/Modules/engines.txt`, then enable the redirect in
`cdeloader.ini` (the loader ships one):

```ini
[loader]
redirect = 1
```

No plugin is required: with the redirect on, the loader indexes `mods/*/Data/**`
and serves your file in place of the game's.
"""

_DATA_MOD_INI = """\
; Metadata for {name} (id {modid}). Kept with the mod for humans and tooling;
; the loader itself needs nothing here -- a Data/ folder is enough.
[mod]
id = {modid}
name = {name}
version = 1.0.0
author = {author}
type = data
"""


def _default_mod_id(name: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "", _slug(name).lower())
    return "org.example." + slug or "org.example.mod"


def scaffold_data_mod(name: str, out_dir: str, mod_id: str = None,
                      author: str = "you", files=None) -> str:
    """Create a data-mod folder (``mods/<slug>/Data/`` + metadata + README)."""
    slug = _slug(name)
    if mod_id is None:
        mod_id = _default_mod_id(name)
    target = os.path.join(out_dir, slug)
    data = os.path.join(target, "Data")
    os.makedirs(data, exist_ok=True)

    for rel in (files or []):
        rel = str(rel).replace("\\", "/").lstrip("/")
        if not rel:
            continue
        dst = os.path.join(data, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(dst):
            with open(dst, "w", encoding="utf-8", newline="\n"):
                pass

    with open(os.path.join(target, "README.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_DATA_README.format(name=name, modid=mod_id, slug=slug))
    with open(os.path.join(target, "mod.ini"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_DATA_MOD_INI.format(name=name, modid=mod_id, author=author))
    return target


def scaffold(name: str, out_dir: str, mod_id: str = None, author: str = "you") -> str:
    slug = _slug(name)
    if mod_id is None:
        mod_id = "org.example." + re.sub(r"[^a-z0-9]+", "", slug.lower()) or "org.example.plugin"
    target = os.path.join(out_dir, slug)
    os.makedirs(target, exist_ok=True)

    with open(os.path.join(target, "plugin.cpp"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_PLUGIN_CPP.format(name=name, modid=mod_id, dir=slug, author=author))
    build = os.path.join(target, "build.sh")
    with open(build, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_BUILD_SH)
    os.chmod(build, 0o755)
    with open(os.path.join(target, "README.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(
            "# %s\n\nA CDE mod-loader plugin (id `%s`).\n\n"
            "Build: `./build.sh`. Install: copy `plugin.dll` to\n"
            "`<game>/mods/%s/plugin.dll`.\n" % (name, mod_id, slug))
    return target

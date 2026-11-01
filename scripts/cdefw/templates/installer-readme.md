# CDE Mod Framework @@VERSION@@ installer

This package installs the mod loader and the mods with a script.

## Requirements

- python
- Children of a Dead Earth

## Install

```sh
python install.py /path/to/CoaDE
```

The installer will rename the game's `vcruntime140.dll` to `vcruntime140_orig.dll` and copy the loader and the mods into the game folder.

## Included mods

@@MODLIST@@

## Script uninstall

```sh
python install.py /path/to/CoaDE --uninstall
```

## Manual uninstall

Delete `cdeloader.dll`, `cdeloader.symbols`, `cdeloader.ini` and `mods`, then rename `vcruntime140_orig.dll` back to `vcruntime140.dll`.
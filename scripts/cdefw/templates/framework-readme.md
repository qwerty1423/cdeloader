# CDE Mod Framework @@VERSION@@

This package is the mod loader. It loads plugins from the game's `mods` folder and draws their menus with F2.

## Requirements

- Children of a Dead Earth

## Install

In the game folder, rename `vcruntime140.dll` to `vcruntime140_orig.dll`, then extract this archive into the game folder.

It should look like this after installing:
```sh
CDE.exe
cdeloader.dll
cdeloader.symbols
cdeloader.ini
vcruntime140.dll
vcruntime140_orig.dll
mods/
```

## Uninstall

Delete `cdeloader.dll`, `cdeloader.symbols`, `cdeloader.ini` and `mods`, then rename `vcruntime140_orig.dll` back to `vcruntime140.dll`.

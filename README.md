# CDE mod loader prototype

warning: llms were used

This repo contains the loader, some mods, and build scripts.

maybe it might be useful

some of the mods are useless but you can disable them rather easily

compatible with the cde community patch.

after install you can use the F2 key to open the gui.

## Mods

- flyby at any range
- laser ablation cap removal
- laser jitter reducer
- laser M^2 cap removal
- manoeuvre node editor

## Requirements

- mingw-w64-gcc
- python
- bash

## Build

```sh
scripts/build-modloader.sh
scripts/build-proxy.sh
```

## Tests

```sh
python -m unittest discover -s scripts/tests
```

# CDE mod loader prototype

This repo contains the loader, some mods, and build scripts.

maybe it might be useful

some of the mods are useless but you can disable them rather easily

compatible with the cde community patch.

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

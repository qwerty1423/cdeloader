# CDE Mod Framework @@VERSION@@ source

This package contains the loader, the mods and the build scripts.

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

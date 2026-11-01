"""Scaffold a new mod folder with the structure the game expects."""

from __future__ import annotations

import os

_README = """# {name}

A Children of a Dead Earth mod.

Layout (see MODDING.md):

    Data/          data files, loaded in the game's fixed step order
    Data/Imports/  shared designs, loaded at the "Imports" step
    Data/Exports/  where the game writes exported designs
    Audio/ Fonts/ Models/ Textures/   assets

To change a stock entry, redefine it **in a file with the same name as the
stock file** (for example `Data/Materials/Minerals.txt`).  Putting an override
in a differently named file makes the stock file load later and replace it.

Validate with:  python3 scripts/cdemod/cli.py lint {name}
"""

_EXAMPLE = """Material MyNewMaterial
Elements\tFe\tO
ElementCount\t1\t1
Density_kg__m3\t5700
EnthalpyOfFormation_kJ__mol\t-272
ThermalConductivity_W__m_K\t3.0
"""


def create_mod(name: str, out_dir: str, example: bool = True) -> str:
    root = os.path.join(out_dir, name)
    for sub in ("Data/Materials", "Data/Imports", "Data/Exports",
                "Audio", "Fonts", "Models", "Textures"):
        os.makedirs(os.path.join(root, sub), exist_ok=True)
    with open(os.path.join(root, "README.md"), "w", encoding="utf-8") as fh:
        fh.write(_README.format(name=name))
    if example:
        with open(os.path.join(root, "Data", "Materials", "MyMaterial.txt"),
                  "w", encoding="utf-8", newline="") as fh:
            fh.write(_EXAMPLE)
    return root

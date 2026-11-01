"""Étape 5 : export des couches en binaire brut pour le module de simulation C++.

Chaque couche est un tableau ligne par ligne depuis le coin nord-ouest, petit-boutiste,
sans en-tête. meta.json décrit la grille. Le module C++ n'a ainsi besoin d'aucune bibliothèque SIG.
"""
import json

from .barriers import BARRIER_WIDTH
from .config import PROCESSED_DIR
from .dem import ASPECT, ELEVATION, SLOPE
from .fuel import FUEL
from .grid import Grid, read

SIM_DIR = PROCESSED_DIR / "sim"
FORMAT_VERSION = 1

# fichier -> (couche source, type numpy)
LAYERS = {
    "fuel.u8": (FUEL, "<u1"),
    "slope.f32": (SLOPE, "<f4"),
    "aspect.f32": (ASPECT, "<f4"),
    "barrier.u8": (BARRIER_WIDTH, "<u1"),
    "elevation.f32": (ELEVATION, "<f4"),
}


def run() -> None:
    grid = Grid.load()
    SIM_DIR.mkdir(parents=True, exist_ok=True)
    for name, (source, dtype) in LAYERS.items():
        data = read(source)
        if data.shape != (grid.height, grid.width):
            raise ValueError(f"{source.name} : {data.shape} au lieu de {(grid.height, grid.width)}")
        data.astype(dtype).tofile(SIM_DIR / name)
        print(f"  écrit : sim/{name}", flush=True)
    meta = {
        "format_version": FORMAT_VERSION,
        "crs": grid.crs,
        "left": grid.left,
        "top": grid.top,
        "res": grid.res,
        "width": grid.width,
        "height": grid.height,
    }
    (SIM_DIR / "meta.json").write_text(json.dumps(meta, indent=2))
    print("  écrit : sim/meta.json", flush=True)

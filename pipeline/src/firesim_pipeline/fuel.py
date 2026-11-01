"""Étape 4 : carte des modèles de combustible.

Priorité : mer (hors relief) > eau BD TOPO > BD Forêt > ESA WorldCover. Les forêts fermées dont le couvert d'arbres
mesuré (étape « canopy ») est trop clair sont ensuite traitées comme des forêts ouvertes.
"""
import numpy as np
import rasterio
import yaml
from rasterio.enums import Resampling
from rasterio.vrt import WarpedVRT

from .barriers import WATER
from .canopy import TREE_COVER
from .config import CONFIG_DIR, PROCESSED_DIR, RAW_DIR
from .dem import ELEVATION
from .grid import Grid, read, write
from .vectors import burn, find, read_all

FUEL = PROCESSED_DIR / "fuel_model.tif"
WATER_MODEL = 98


def _worldcover(grid: Grid) -> np.ndarray:
    """Classe WorldCover majoritaire de chaque cellule (0 = pas de donnée)."""
    out = np.zeros((grid.height, grid.width), dtype="uint8")
    for tif in find("worldcover", "*.tif"):
        with rasterio.open(tif) as src, WarpedVRT(
            src, crs=grid.crs, transform=grid.transform, width=grid.width, height=grid.height,
            resampling=Resampling.mode, nodata=0,
        ) as vrt:
            data = vrt.read(1)
        out = np.where(out == 0, data, out)
        print(f"  WorldCover : {tif.name}", flush=True)
    return out


def _bdforet(grid: Grid, mapping: dict[str, int], default: int) -> np.ndarray:
    shps = [p for p in find("bdforet", "*.shp") if "FORMATION_VEGETALE" in p.name.upper()]
    forest = read_all(shps, grid.crs, columns=["CODE_TFV"])
    codes = forest["code_tfv"].str.strip()
    unknown = sorted(set(codes) - set(mapping))
    if unknown:
        print(f"  codes BD Forêt non prévus (modèle {default} appliqué) : {unknown}", flush=True)
    models = codes.map(mapping).fillna(default).astype(int)
    print(f"  BD Forêt : {len(forest)} polygones", flush=True)
    return burn(grid, zip(forest.geometry, models), "uint8")


def run() -> None:
    grid = Grid.load()
    cfg = yaml.safe_load((CONFIG_DIR / "fuel_models.yaml").read_text(encoding="utf-8"))

    lut = np.zeros(256, dtype="uint8")
    for cls, model in cfg["worldcover"].items():
        lut[int(cls)] = model
    fuel = lut[_worldcover(grid)]

    forest = _bdforet(grid, cfg["bdforet"], cfg["bdforet_default"])
    fuel = np.where(forest > 0, forest, fuel)

    clear = cfg["foret_claire"]
    sparse = np.isin(fuel, clear["modeles"]) & (read(TREE_COVER) < clear["couvert_min"])
    print(f"  forêts fermées à moins de {clear['couvert_min']} % d'arbres mesurés : "
          f"{100 * np.count_nonzero(sparse) / np.count_nonzero(np.isin(fuel, clear['modeles'])):.0f} %, "
          f"passées en {cfg['models'][clear['modele']]['code']}", flush=True)
    fuel[sparse] = clear["modele"]
    fuel[read(WATER) == 1] = WATER_MODEL

    land = ~np.isnan(read(ELEVATION))
    gaps = np.count_nonzero(land & (fuel == 0))
    if gaps:
        print(f"  {gaps} cellules à terre sans végétation connue : classées sol nu", flush=True)
        fuel[land & (fuel == 0)] = 99
    fuel[~land] = 0

    write(grid, FUEL, fuel, None, description="modèle de combustible Scott & Burgan (code numérique)")

    known = cfg["models"]
    values, counts = np.unique(fuel[land], return_counts=True)
    for v, n in sorted(zip(values, counts), key=lambda vc: -vc[1]):
        label = known.get(int(v), {"code": "?", "name": "inconnu"})
        print(f"  {label['code']:>4} {label['name']:<40} {100 * n / land.sum():5.1f} %")

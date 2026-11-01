"""Lecture des données vectorielles brutes (BD Forêt, BD TOPO) et rasterisation sur la grille."""
import warnings
from collections.abc import Iterable
from pathlib import Path

import geopandas as gpd
import numpy as np
import pandas as pd
import pyogrio
from rasterio.features import rasterize
from shapely.geometry.base import BaseGeometry

from .config import RAW_DIR
from .grid import Grid

# Les GPKG de la BD TOPO déclarent des types PostgreSQL que GDAL signale sans conséquence.
warnings.filterwarnings("ignore", message="Field format .* not supported", category=RuntimeWarning)


def find(group: str, pattern: str) -> list[Path]:
    paths = sorted((RAW_DIR / group).rglob(pattern))
    if not paths:
        raise FileNotFoundError(f"aucun fichier {pattern} dans {RAW_DIR / group} : lance d'abord « download »")
    return paths


def read_all(
    paths: list[Path], crs: str, columns: list[str], layer: str | None = None, where: str | None = None
) -> gpd.GeoDataFrame:
    """Lit et concatène les fichiers des deux départements. Les noms de colonnes sont
    cherchés sans tenir compte de la casse et rendus en minuscules."""
    frames = []
    for p in paths:
        fields = {f.lower(): f for f in pyogrio.read_info(p, layer=layer)["fields"]}
        missing = [c for c in columns if c.lower() not in fields]
        if missing:
            raise KeyError(f"{p.name} : colonnes absentes {missing}")
        df = pyogrio.read_dataframe(p, layer=layer, columns=[fields[c.lower()] for c in columns], where=where)
        df = df.rename(columns={fields[c.lower()]: c.lower() for c in columns})
        frames.append(df.to_crs(crs))
    return gpd.GeoDataFrame(pd.concat(frames, ignore_index=True), crs=crs)


def burn(grid: Grid, shapes: Iterable[tuple[BaseGeometry, int]], dtype: str, all_touched: bool = False) -> np.ndarray:
    """Rasterise des (géométrie, valeur) ; en cas de recouvrement, la dernière gagne."""
    shapes = [(g, v) for g, v in shapes if g is not None and not g.is_empty]
    if not shapes:
        return np.zeros((grid.height, grid.width), dtype=dtype)
    return rasterize(
        shapes,
        out_shape=(grid.height, grid.width),
        transform=grid.transform,
        fill=0,
        dtype=dtype,
        all_touched=all_touched,
    )

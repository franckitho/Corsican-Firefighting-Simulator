"""Étape 2 : relief.

Assemble les dalles RGE ALTI 5 m des deux départements en une seule mosaïque
(utilisée plus tard comme terrain 3D dans Cesium), définit la grille de simulation
à 25 m sur la même emprise, puis calcule altitude, pente et orientation à 25 m.
"""
import math
import warnings
from collections import defaultdict
from pathlib import Path

import numpy as np
import rasterio
import rasterio.shutil
from rasterio.windows import Window

from .config import DEM_RES, PROCESSED_DIR, RAW_DIR, SIM_RES
from .grid import Grid, write

NODATA = -99999.0
# Une cellule de 25 m regroupe 5 x 5 points à 5 m ; en dessous de 13 points valides
# (bord de mer surtout), elle est considérée hors terre.
MIN_VALID = 13
FLAT_SLOPE_DEG = 0.5

DEM_5M = PROCESSED_DIR / "terrain" / "dem_5m.tif"
ELEVATION = PROCESSED_DIR / "elevation.tif"
SLOPE = PROCESSED_DIR / "slope.tif"
ASPECT = PROCESSED_DIR / "aspect.tif"


def _asc_header(path: Path) -> dict[str, float]:
    header = {}
    with path.open() as f:
        for _ in range(6):
            key, value = f.readline().split()
            header[key.lower()] = float(value)
    cs = header["cellsize"]
    if "xllcenter" in header:
        header["xllcorner"] = header["xllcenter"] - cs / 2
        header["yllcorner"] = header["yllcenter"] - cs / 2
    return header


def _tiles() -> list[tuple[Path, dict[str, float]]]:
    paths = sorted(p for p in (RAW_DIR / "rgealti").rglob("*.asc") if "MNT" in p.name.upper())
    if not paths:
        raise FileNotFoundError("aucune dalle RGE ALTI : lance d'abord l'étape « download »")
    tiles = [(p, _asc_header(p)) for p in paths]
    for p, h in tiles:
        if abs(h["cellsize"] - DEM_RES) > 1e-6:
            raise ValueError(f"{p.name} : pas de {h['cellsize']} m au lieu de {DEM_RES}")
    return tiles


def _sim_grid(tiles) -> Grid:
    left = min(h["xllcorner"] for _, h in tiles)
    bottom = min(h["yllcorner"] for _, h in tiles)
    right = max(h["xllcorner"] + h["ncols"] * DEM_RES for _, h in tiles)
    top = max(h["yllcorner"] + h["nrows"] * DEM_RES for _, h in tiles)
    return Grid(
        left=left,
        top=top,
        res=SIM_RES,
        width=math.ceil((right - left) / SIM_RES),
        height=math.ceil((top - bottom) / SIM_RES),
    )


def _mosaic(tiles, grid5: Grid, path: Path) -> None:
    """Écrit la mosaïque 5 m non compressée (fichier de travail)."""
    # Une même dalle peut être livrée dans les deux départements : on fusionne
    # les versions d'un même emplacement avant d'écrire, une seule fois.
    by_slot: dict[tuple[int, int], list[Path]] = defaultdict(list)
    for p, h in tiles:
        col = (h["xllcorner"] - grid5.left) / DEM_RES
        row = (grid5.top - (h["yllcorner"] + h["nrows"] * DEM_RES)) / DEM_RES
        if abs(col - round(col)) > 1e-6 or abs(row - round(row)) > 1e-6:
            raise ValueError(f"{p.name} n'est pas alignée sur la grille 5 m")
        by_slot[(round(row), round(col))].append(p)

    profile = grid5.profile("float32", NODATA) | {"compress": None, "predictor": None, "BIGTIFF": "YES"}
    with rasterio.open(path, "w", **{k: v for k, v in profile.items() if v is not None}) as dst:
        for i, ((row, col), paths) in enumerate(sorted(by_slot.items())):
            merged = None
            for p in paths:
                with rasterio.open(p) as src:
                    data = src.read(1).astype("float32")
                    src_nodata = src.nodata if src.nodata is not None else NODATA
                data[data == src_nodata] = NODATA
                merged = data if merged is None else np.where(merged == NODATA, data, merged)
            dst.write(merged, 1, window=Window(col, row, merged.shape[1], merged.shape[0]))
            if i % 50 == 0:
                print(f"  dalles : {i} / {len(by_slot)}", flush=True)


def _downsample(path5: Path, grid: Grid) -> np.ndarray:
    """Moyenne des 25 points à 5 m de chaque cellule de 25 m."""
    f = round(SIM_RES / DEM_RES)
    out = np.full((grid.height, grid.width), np.nan, dtype="float32")
    step = 256  # lignes de 25 m traitées à la fois
    with rasterio.open(path5) as src:
        for r0 in range(0, grid.height, step):
            r1 = min(r0 + step, grid.height)
            block = src.read(1, window=Window(0, r0 * f, grid.width * f, (r1 - r0) * f))
            block = np.where(block == NODATA, np.nan, block)
            block = block.reshape(r1 - r0, f, grid.width, f)
            valid = np.count_nonzero(~np.isnan(block), axis=(1, 3))
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", RuntimeWarning)  # cellules entièrement en mer
                mean = np.nanmean(block, axis=(1, 3))
            out[r0:r1] = np.where(valid >= MIN_VALID, mean, np.nan)
    return out


def slope_aspect(z: np.ndarray, res: float) -> tuple[np.ndarray, np.ndarray]:
    """Pente (degrés) et orientation (degrés depuis le nord, sens horaire, direction
    vers laquelle la pente descend ; -1 si terrain plat) par la méthode de Horn."""
    pad = np.pad(z, 1, mode="edge")
    center = pad[1:-1, 1:-1]

    def nb(dr: int, dc: int) -> np.ndarray:
        v = pad[1 + dr : pad.shape[0] - 1 + dr, 1 + dc : pad.shape[1] - 1 + dc]
        return np.where(np.isnan(v), center, v)  # voisin hors terre : on prend le centre

    a, b, c = nb(-1, -1), nb(-1, 0), nb(-1, 1)
    d, f = nb(0, -1), nb(0, 1)
    g, h, i = nb(1, -1), nb(1, 0), nb(1, 1)
    dz_east = ((c + 2 * f + i) - (a + 2 * d + g)) / (8 * res)
    dz_north = ((a + 2 * b + c) - (g + 2 * h + i)) / (8 * res)

    slope = np.degrees(np.arctan(np.hypot(dz_east, dz_north))).astype("float32")
    aspect = (np.degrees(np.arctan2(-dz_east, -dz_north)) % 360).astype("float32")
    aspect[aspect >= 360] = 0  # arrondi float32 de -0.0001 % 360
    aspect[slope < FLAT_SLOPE_DEG] = -1
    land = ~np.isnan(z)
    return np.where(land, slope, np.nan), np.where(land, aspect, np.nan)


def run() -> None:
    tiles = _tiles()
    grid = _sim_grid(tiles)
    grid.save()
    print(f"  {len(tiles)} dalles, grille {grid.width} x {grid.height} cellules de {SIM_RES:.0f} m", flush=True)

    tmp = PROCESSED_DIR / "terrain" / "_dem_5m_work.tif"
    tmp.parent.mkdir(parents=True, exist_ok=True)
    _mosaic(tiles, grid.scaled(DEM_RES), tmp)

    elevation = _downsample(tmp, grid)
    slope, aspect = slope_aspect(elevation, grid.res)
    tags = {"source": "IGN RGE ALTI 5 m", "vertical_datum": "IGN78C"}
    write(grid, ELEVATION, elevation, np.nan, units="m", **tags)
    write(grid, SLOPE, slope, np.nan, units="degrees")
    write(grid, ASPECT, aspect, np.nan, units="degrees clockwise from north, downslope direction; -1 = flat")

    print("  compression de la mosaïque 5 m (terrain 3D)…", flush=True)
    rasterio.shutil.copy(
        tmp, DEM_5M, driver="COG", COMPRESS="DEFLATE", PREDICTOR="YES",
        RESAMPLING="AVERAGE", BIGTIFF="IF_SAFER", NUM_THREADS="ALL_CPUS",
    )
    tmp.unlink()
    print(f"  écrit : {DEM_5M.name}", flush=True)

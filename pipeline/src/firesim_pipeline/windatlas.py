"""Étape « wind » : atlas des vents corrigés par le relief, calculé avec WindNinja.

Pour chacune des 16 directions de vent général, WindNinja (USFS, solveur à conservation de la masse)
calcule le vent à 10 m sur toute l'île pour un vent d'entrée de 10 m/s. Ce solveur est linéaire :
le rapport vent local / vent général ne dépend pas de la vitesse, d'où un seul calcul par direction.

Le résultat (data/processed/sim/wind/) donne, par direction et sur une grille de 200 m, les composantes
est et nord du vent local pour un vent général de vitesse 1. Calcul long (plusieurs minutes par direction),
reprise possible : les directions déjà calculées sont conservées.
"""
import json
import math
import os
import subprocess
import time
from pathlib import Path

import numpy as np
import rasterio
from rasterio.enums import Resampling
from rasterio.warp import reproject

from .config import PROCESSED_DIR
from .dem import DEM_5M
from .export import SIM_DIR
from .grid import Grid

ATLAS_DIR = SIM_DIR / "wind"
WORK_DIR = PROCESSED_DIR / "windninja"
FORMAT_VERSION = 1

DIRECTIONS = 16
INPUT_SPEED = 10.0     # m/s
DEM_RES = 100.0        # relief fourni à WindNinja (m)
MESH_RES = 200.0       # maillage du calcul (m)
ATLAS_RES = 200.0      # grille de l'atlas (m), multiple du pas de simulation
VEGETATION = "brush"   # rugosité : maquis


def find_cli() -> Path:
    if env := os.environ.get("FIRESIM_WINDNINJA"):
        return Path(env)
    for root in (Path(os.environ.get("ProgramFiles", r"C:\Program Files")), Path(r"C:\WindNinja")):
        found = sorted(root.glob("WindNinja*/**/WindNinja_cli.exe"))
        if found:
            return found[-1]
    raise FileNotFoundError("WindNinja_cli.exe introuvable : installer WindNinja ou définir FIRESIM_WINDNINJA")


def prepare_dem(grid: Grid) -> Path:
    """Relief à 100 m, mer à 0 m (WindNinja refuse les trous)."""
    path = WORK_DIR / "corse_dem_100m.tif"
    if path.exists():
        return path
    factor = round(DEM_RES / grid.res)
    shape = (math.ceil(grid.height / factor), math.ceil(grid.width / factor))
    with rasterio.open(DEM_5M) as src:
        dem = src.read(1, out_shape=shape, resampling=Resampling.average, masked=True).filled(0).astype("float32")
        transform = src.transform * src.transform.scale(src.width / shape[1], src.height / shape[0])
        crs = src.crs
    dem[~np.isfinite(dem) | (dem < -100)] = 0
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    profile = {"driver": "GTiff", "dtype": "float32", "count": 1, "width": shape[1], "height": shape[0],
               "crs": crs, "transform": transform}
    with rasterio.open(path, "w", **profile) as dst:
        dst.write(dem, 1)
    print(f"  relief pour WindNinja : {shape[1]} x {shape[0]} cellules de {DEM_RES:.0f} m", flush=True)
    return path


def run_direction(cli: Path, dem: Path, direction: float) -> tuple[Path, Path]:
    out_dir = WORK_DIR / f"dir_{direction:05.1f}"
    vel = sorted(out_dir.glob("*_vel.asc"))
    ang = sorted(out_dir.glob("*_ang.asc"))
    if vel and ang:
        return vel[0], ang[0]
    out_dir.mkdir(parents=True, exist_ok=True)
    cfg = out_dir / "windninja.cfg"
    cfg.write_text("\n".join([
        f"num_threads = {os.cpu_count() or 4}",
        f"elevation_file = {dem}",
        "initialization_method = domainAverageInitialization",
        "time_zone = Europe/Paris",
        f"input_speed = {INPUT_SPEED}",
        "input_speed_units = mps",
        "output_speed_units = mps",
        f"input_direction = {direction}",
        "input_wind_height = 10.0",
        "units_input_wind_height = m",
        "output_wind_height = 10.0",
        "units_output_wind_height = m",
        f"vegetation = {VEGETATION}",
        f"mesh_resolution = {MESH_RES}",
        "units_mesh_resolution = m",
        "write_ascii_output = true",
        f"output_path = {out_dir}",
        "",
    ]))
    start = time.monotonic()
    with (out_dir / "windninja.log").open("w") as log:
        subprocess.run([str(cli), str(cfg)], stdout=log, stderr=subprocess.STDOUT, check=True)
    vel = sorted(out_dir.glob("*_vel.asc"))
    ang = sorted(out_dir.glob("*_ang.asc"))
    if not (vel and ang):
        raise RuntimeError(f"WindNinja n'a pas produit de grilles ASCII, voir {out_dir / 'windninja.log'}")
    print(f"  direction {direction:5.1f}° : {time.monotonic() - start:.0f} s", flush=True)
    return vel[0], ang[0]


def to_components(vel_path: Path, ang_path: Path, atlas: Grid, direction: float) -> tuple[np.ndarray, np.ndarray]:
    """Composantes est/nord du vent local pour un vent général unité, sur la grille de l'atlas."""
    with rasterio.open(vel_path) as v, rasterio.open(ang_path) as a:
        speed = v.read(1, masked=True).filled(np.nan) / INPUT_SPEED
        came_from = np.radians(a.read(1, masked=True).filled(np.nan))  # direction d'où vient le vent
        src_transform = v.transform
        src_crs = v.crs or atlas.crs
    out = []
    for component in (-speed * np.sin(came_from), -speed * np.cos(came_from)):
        dst = np.full((atlas.height, atlas.width), np.nan, dtype="float32")
        reproject(component.astype("float32"), dst, src_transform=src_transform, src_crs=src_crs,
                  dst_transform=atlas.transform, dst_crs=atlas.crs, resampling=Resampling.bilinear,
                  src_nodata=np.nan, dst_nodata=np.nan)
        out.append(dst)
    # Hors du domaine de WindNinja : vent général inchangé
    toward = math.radians(direction + 180)
    for dst, fallback in zip(out, (math.sin(toward), math.cos(toward))):
        dst[~np.isfinite(dst)] = fallback
    return out[0], out[1]


def run() -> None:
    grid = Grid.load()
    factor = round(ATLAS_RES / grid.res)
    atlas = Grid(left=grid.left, top=grid.top, res=ATLAS_RES,
                 width=math.ceil(grid.width / factor), height=math.ceil(grid.height / factor))
    cli = find_cli()
    print(f"  WindNinja : {cli}", flush=True)
    dem = prepare_dem(grid)

    east = np.empty((DIRECTIONS, atlas.height, atlas.width), dtype="<f4")
    north = np.empty_like(east)
    for k in range(DIRECTIONS):
        direction = k * 360.0 / DIRECTIONS
        vel, ang = run_direction(cli, dem, direction)
        east[k], north[k] = to_components(vel, ang, atlas, direction)

    ATLAS_DIR.mkdir(parents=True, exist_ok=True)
    east.tofile(ATLAS_DIR / "east.f32")
    north.tofile(ATLAS_DIR / "north.f32")
    meta = {"format_version": FORMAT_VERSION, "crs": atlas.crs, "left": atlas.left, "top": atlas.top,
            "res": atlas.res, "width": atlas.width, "height": atlas.height, "directions": DIRECTIONS,
            "input_speed_mps": INPUT_SPEED, "vegetation": VEGETATION, "mesh_res_m": MESH_RES}
    (ATLAS_DIR / "meta.json").write_text(json.dumps(meta, indent=2))
    factors = np.hypot(east, north)
    print(f"  atlas écrit : {DIRECTIONS} directions, {atlas.width} x {atlas.height} cellules de {ATLAS_RES:.0f} m ; "
          f"facteur de vitesse de {np.percentile(factors, 1):.2f} à {np.percentile(factors, 99):.2f} (1er-99e centile)",
          flush=True)
    preview(atlas, east, north)


def preview(atlas: Grid, east: np.ndarray, north: np.ndarray, directions=(225.0, 315.0)) -> None:
    """Carte du vent local pour quelques directions : facteur de vitesse en couleur, flèches tous les 4 km."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    from .quicklook import OUT, hillshade

    with rasterio.open(DEM_5M) as src:
        elev = src.read(1, out_shape=(atlas.height, atlas.width), resampling=Resampling.average, masked=True)
    land = ~np.ma.getmaskarray(elev) & (elev.filled(0) > -100)
    shade = hillshade(np.where(land, elev.filled(0), 0), atlas.res)

    fig, axes = plt.subplots(1, len(directions), figsize=(8 * len(directions), 13))
    step = 20  # une flèche toutes les 20 cellules (4 km)
    rows, cols = np.mgrid[step // 2 : atlas.height : step, step // 2 : atlas.width : step]
    for ax, direction in zip(np.atleast_1d(axes), directions):
        k = round(direction / (360.0 / DIRECTIONS)) % DIRECTIONS
        factor = np.hypot(east[k], north[k])
        ax.imshow(shade, cmap="gray", vmin=-0.2, vmax=1.2)
        im = ax.imshow(np.ma.masked_where(~land, factor), cmap="RdYlBu_r", vmin=0.3, vmax=1.7, alpha=0.65)
        sel = land[rows, cols]
        ax.quiver(cols[sel], rows[sel], east[k][rows, cols][sel], north[k][rows, cols][sel],
                  scale=40, width=0.0025, color="black")
        ax.set_title(f"Vent général venant de {k * 360 / DIRECTIONS:g}° : vitesse locale / vitesse générale", fontsize=11)
        ax.set_axis_off()
        fig.colorbar(im, ax=ax, shrink=0.5)
    OUT.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT / "wind_atlas.png", dpi=90, bbox_inches="tight")
    plt.close(fig)
    print("  écrit : quicklook/wind_atlas.png", flush=True)

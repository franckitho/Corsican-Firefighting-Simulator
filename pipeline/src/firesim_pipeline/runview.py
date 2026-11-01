"""Visualise une simulation du module C++ : GeoTIFF (pour un logiciel SIG) et aperçu PNG
avec la zone brûlée colorée par heure d'arrivée et les contours horaires, sur le relief.

    python -I -m firesim_pipeline.runview runs/mon_feu
"""
import argparse
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import rasterio
from affine import Affine
from rasterio.windows import Window

from .dem import ELEVATION
from .fuel import FUEL
from .quicklook import _fuel_rgb, hillshade


def _load(run_dir: Path) -> tuple[dict, dict[str, np.ndarray], Affine]:
    run = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
    g, w = run["grid"], run["window"]
    shape = (w["height"], w["width"])
    layers = {name: np.fromfile(run_dir / f, dtype="<f4").reshape(shape) for name, f in run["layers"].items()}
    transform = Affine(g["res"], 0, g["left"] + w["col"] * g["res"], 0, -g["res"], g["top"] - w["row"] * g["res"])
    return run, layers, transform


def _write_geotiffs(run_dir: Path, run: dict, layers: dict[str, np.ndarray], transform: Affine) -> None:
    for name, data in layers.items():
        profile = {
            "driver": "GTiff", "dtype": "float32", "count": 1, "width": data.shape[1], "height": data.shape[0],
            "crs": run["grid"]["crs"], "transform": transform, "nodata": np.nan, "compress": "deflate",
        }
        with rasterio.open(run_dir / f"{name}.tif", "w", **profile) as dst:
            dst.write(data, 1)


def _preview(run_dir: Path, run: dict, arrival: np.ndarray, margin_m: float) -> Path:
    g, w = run["grid"], run["window"]
    res = g["res"]
    m = int(margin_m // res)
    c0, r0 = max(0, w["col"] - m), max(0, w["row"] - m)
    c1, r1 = min(g["width"], w["col"] + w["width"] + m), min(g["height"], w["row"] + w["height"] + m)
    window = Window(c0, r0, c1 - c0, r1 - r0)
    with rasterio.open(ELEVATION) as src:
        elev = src.read(1, window=window)
    with rasterio.open(FUEL) as src:
        fuel = src.read(1, window=window)

    hours = np.full(elev.shape, np.nan, dtype="float32")
    hours[w["row"] - r0 : w["row"] - r0 + w["height"], w["col"] - c0 : w["col"] - c0 + w["width"]] = arrival / 60
    shade = hillshade(elev, res)
    background = _fuel_rgb(fuel) * (0.55 + 0.45 * shade[..., None])

    fig, ax = plt.subplots(figsize=(11, 11 * elev.shape[0] / elev.shape[1] + 1))
    ax.imshow(background)
    im = ax.imshow(np.ma.masked_invalid(hours), cmap="inferno_r", alpha=0.8, vmin=0, vmax=max(1.0, np.nanmax(hours)))
    fig.colorbar(im, ax=ax, shrink=0.6, label="heure d'arrivée du feu (h)")
    duration_h = run["options"]["duration_min"] / 60
    step = 0.5 if duration_h <= 3 else 1.0
    levels = np.arange(step, np.nanmax(hours) + 1e-6, step)
    if len(levels):
        cs = ax.contour(np.nan_to_num(hours, nan=1e6), levels=levels, colors="white", linewidths=0.8)
        ax.clabel(cs, fmt=lambda v: f"{v:g} h", fontsize=8)

    for ig in run["ignitions"]:
        ax.plot((ig["x"] - g["left"]) / res - c0 - 0.5, (g["top"] - ig["y"]) / res - r0 - 0.5,
                marker="*", color="cyan", markersize=16, markeredgecolor="black")

    # Une flèche par période de vent, de gauche à droite
    for i, period in enumerate(run["weather"]):
        x0 = 0.06 + 0.11 * i
        toward = np.radians(period["wind_from_deg"] + 180)
        ax.annotate("", xy=(x0 + 0.04 * np.sin(toward), 0.92 + 0.04 * np.cos(toward)), xytext=(x0, 0.92),
                    xycoords="axes fraction", arrowprops={"arrowstyle": "->", "color": "white", "lw": 2.5})
        label = f"{period['wind_kmh']:g} km/h" + (f"\ndès {period['start_min'] / 60:g} h" if len(run["weather"]) > 1 else "")
        ax.text(x0, 0.85, label, transform=ax.transAxes, color="white", ha="center", va="top", fontsize=8,
                bbox={"facecolor": "black", "alpha": 0.5, "pad": 2})
    weather = run["weather"][0]

    km = 1000 / res
    ax.plot([0.04 * elev.shape[1], 0.04 * elev.shape[1] + km], [0.96 * elev.shape[0]] * 2, color="white", lw=3)
    ax.text(0.04 * elev.shape[1] + km / 2, 0.94 * elev.shape[0], "1 km", color="white", ha="center", fontsize=9)

    s = run["summary"]
    wind_kind = "vent corrigé par le relief" if run["options"].get("terrain_wind") else "vent uniforme"
    ax.set_title(f"{s['area_ha']:.0f} ha brûlés en {duration_h:g} h — vitesse max {s['max_ros_m_min'] * 0.06:.1f} km/h, "
                 f"flammes jusqu'à {s['max_flame_m']:.0f} m\nhumidité 1 h {weather['m1h'] * 100:g} % — {wind_kind}",
                 fontsize=10)
    ax.set_axis_off()
    out = run_dir / "apercu.png"
    fig.savefig(out, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return out


def main(argv: list[str] | None = None) -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(prog="runview", description="Visualise une simulation de feu.")
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--margin", type=float, default=1500, help="marge autour du feu, en mètres")
    args = parser.parse_args(argv)
    run, layers, transform = _load(args.run_dir)
    _write_geotiffs(args.run_dir, run, layers, transform)
    print(f"  écrit : {', '.join(f'{n}.tif' for n in layers)}")
    print(f"  écrit : {_preview(args.run_dir, run, layers['arrival_min'], args.margin).name}")


if __name__ == "__main__":
    main()

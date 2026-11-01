"""Étape 5 : aperçus PNG pour vérifier les couches à l'œil."""
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import yaml
from matplotlib.colors import ListedColormap
from matplotlib.patches import Patch

from .barriers import BARRIER_WIDTH, WATER
from .config import CONFIG_DIR, PROCESSED_DIR
from .dem import ELEVATION, SLOPE
from .fuel import FUEL
from .grid import Grid, read

OUT = PROCESSED_DIR / "quicklook"
OVERVIEW_STEP = 4   # 1 pixel = 100 m sur la vue d'ensemble
ZOOM_CELLS = 600    # zoom de 15 km de côté au centre de l'île, à pleine résolution

FUEL_COLORS = {
    0: "#dfe9f3", 91: "#7f7f7f", 92: "#ffffff", 93: "#c8c8a0", 98: "#4a90d9", 99: "#c9b99a",
    101: "#f3e79b", 102: "#e6c94f", 122: "#c6b04a", 145: "#d9822b", 147: "#b5521b",
    165: "#2f6b2f", 182: "#8fc77e", 183: "#5c9e6e", 186: "#6fae4f", 188: "#3f8f5f",
}


def hillshade(z: np.ndarray, res: float, azimuth: float = 315, altitude: float = 45) -> np.ndarray:
    z = np.nan_to_num(z, nan=0.0)
    dz_north, dz_east = np.gradient(z, res)
    dz_north = -dz_north  # les lignes vont vers le sud
    slope = np.arctan(np.hypot(dz_east, dz_north))
    aspect = np.arctan2(-dz_east, -dz_north)
    az, alt = np.radians(azimuth), np.radians(altitude)
    shade = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(shade, 0, 1)


def _save(fig, name: str) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT / name, dpi=100, bbox_inches="tight")
    plt.close(fig)
    print(f"  écrit : quicklook/{name}", flush=True)


def _fuel_rgb(fuel: np.ndarray) -> np.ndarray:
    lut = np.zeros((256, 3))
    for code, color in FUEL_COLORS.items():
        lut[code] = matplotlib.colors.to_rgb(color)
    return lut[fuel]


def _panel(ax, title: str) -> None:
    ax.set_title(title, fontsize=11)
    ax.set_axis_off()


def _layers(elev, slope, fuel, water, barrier, res: float, name: str, models: dict) -> None:
    shade = hillshade(elev, res)
    land = ~np.isnan(elev)

    fig, axes = plt.subplots(1, 4, figsize=(22, 12))
    ax = axes[0]
    # vmin négatif : saute la partie bleue de « terrain », réservée à l'eau
    ax.imshow(np.where(land, elev, np.nan), cmap="terrain", vmin=-900, vmax=2700)
    ax.imshow(shade, cmap="gray", alpha=0.35)
    _panel(ax, "Altitude (0–2700 m) + ombrage")

    ax = axes[1]
    im = ax.imshow(slope, cmap="magma_r", vmin=0, vmax=45)
    fig.colorbar(im, ax=ax, shrink=0.4, label="pente (°)")
    _panel(ax, "Pente")

    ax = axes[2]
    ax.imshow(_fuel_rgb(fuel) * (0.65 + 0.35 * shade[..., None]))
    present = [c for c in np.unique(fuel) if c in FUEL_COLORS and c != 0]
    ax.legend(
        handles=[Patch(color=FUEL_COLORS[c], label=f"{models[c]['code']} {models[c]['name']}") for c in present],
        loc="lower left", fontsize=7, framealpha=0.9,
    )
    _panel(ax, "Combustible (Scott & Burgan)")

    ax = axes[3]
    ax.imshow(shade, cmap="gray", vmin=-0.3, vmax=1.2)
    ax.imshow(np.ma.masked_equal(water, 0), cmap=ListedColormap(["#1f6fd1"]))
    ax.imshow(np.ma.masked_equal(barrier, 0), cmap="autumn_r", vmin=0, vmax=15)
    _panel(ax, "Eau (bleu), coupures (jaune → rouge = 0 → 15 m)")

    _save(fig, name)


def run() -> None:
    grid = Grid.load()
    models = {int(k): v for k, v in yaml.safe_load((CONFIG_DIR / "fuel_models.yaml").read_text(encoding="utf-8"))["models"].items()}
    elev, slope, fuel = read(ELEVATION), read(SLOPE), read(FUEL)
    water, barrier = read(WATER), read(BARRIER_WIDTH)

    s = slice(None, None, OVERVIEW_STEP)
    _layers(elev[s, s], slope[s, s], fuel[s, s], water[s, s], barrier[s, s],
            grid.res * OVERVIEW_STEP, "overview.png", models)

    r0, c0 = (grid.height - ZOOM_CELLS) // 2, (grid.width - ZOOM_CELLS) // 2
    z = (slice(r0, r0 + ZOOM_CELLS), slice(c0, c0 + ZOOM_CELLS))
    _layers(elev[z], slope[z], fuel[z], water[z], barrier[z], grid.res, "zoom_center.png", models)

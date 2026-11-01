"""Rejeu de feux réels : le simulateur part du même point, à la même heure, avec la météo du jour, et son résultat
est comparé à la zone brûlée cartographiée par EFFIS (Copernicus) sur images satellite.

    python -I -m firesim_pipeline.rejeu                  # feux du catalogue
    python -I -m firesim_pipeline.rejeu cagnano-2026     # rejoue un feu (ou « all »)

Catalogue : pipeline/config/feux_historiques.yaml. Pour chaque feu :
- runs/rejeu/<feu>/ : météo (meteo.csv), simulation (sim/), comparaison.json et comparaison.png ;
- data/processed/feux/<feu>.json et <feu>_contour.u8 : le feu et son contour observé, pour le projet Unreal.
Il faut l'outil firesim compilé (sim/build.bat).
"""
import argparse
import json
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

import geopandas as gpd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import rasterio
import requests
import yaml
from rasterio import features
from rasterio.windows import Window
from rasterio.windows import transform as window_transform
from shapely.geometry.base import BaseGeometry

from . import meteo
from .config import CONFIG_DIR, PROCESSED_DIR, PROJECT_ROOT, RAW_DIR
from .dem import ELEVATION
from .fuel import FUEL
from .grid import Grid
from .quicklook import _fuel_rgb, hillshade

CATALOGUE = CONFIG_DIR / "feux_historiques.yaml"
OUT_DIR = PROCESSED_DIR / "feux"
RUNS_DIR = PROJECT_ROOT / "runs" / "rejeu"
EFFIS_WFS = "https://maps.effis.emergency.copernicus.eu/effis"
OUTLINE_CELLS = 2   # largeur du contour dessiné dans Unreal (cellules de 25 m)
MARGIN_CELLS = 40   # marge autour des feux sur les comparaisons


def catalogue() -> dict:
    return yaml.safe_load(CATALOGUE.read_text(encoding="utf-8"))


def observed_area(fire_id: str, entry: dict) -> BaseGeometry:
    """Zone brûlée observée (Lambert 93), téléchargée une fois sur EFFIS."""
    cache = RAW_DIR / "effis" / f"{fire_id}.geojson"
    if not cache.exists():
        lat, lon = entry["point"]
        wanted = str(entry["contour"]["id"])
        cache.parent.mkdir(parents=True, exist_ok=True)
        gml = cache.with_suffix(".gml")
        # Les feux des années passées sont dans la couche de leur année, ceux de l'année en cours dans « season ».
        for layer in (f"modis.ba.poly.{entry['contour']['annee']}", "modis.ba.poly.season"):
            params = {"service": "WFS", "version": "1.0.0", "request": "GetFeature", "typeName": f"ms:{layer}",
                      "BBOX": f"{lon - 0.2},{lat - 0.2},{lon + 0.2},{lat + 0.2}", "SRSNAME": "EPSG:4326"}
            gml.write_bytes(requests.get(EFFIS_WFS, params=params, timeout=120).content)
            try:
                found = gpd.read_file(gml)
            except Exception:  # couche inconnue ou réponse vide
                continue
            if "id" in found and (found := found[found["id"].astype(str) == wanted]).size:
                found.set_crs(4326, allow_override=True).to_file(cache, driver="GeoJSON")
                break
        gml.unlink(missing_ok=True)
        if not cache.exists():
            raise RuntimeError(f"zone brûlée EFFIS n° {wanted} introuvable")
    return gpd.read_file(cache).to_crs(2154).geometry.union_all()


def _cells(grid: Grid, bounds: tuple[float, float, float, float]) -> tuple[int, int, int, int]:
    """Colonnes et lignes (début, fin exclue) de la grille qui couvrent cette emprise Lambert 93."""
    minx, miny, maxx, maxy = bounds
    return (int(np.floor((minx - grid.left) / grid.res)), int(np.floor((grid.top - maxy) / grid.res)),
            int(np.ceil((maxx - grid.left) / grid.res)), int(np.ceil((grid.top - miny) / grid.res)))


def _window(grid: Grid, c0: int, r0: int, c1: int, r1: int, margin: int) -> Window:
    c0, r0 = max(0, c0 - margin), max(0, r0 - margin)
    c1, r1 = min(grid.width, c1 + margin), min(grid.height, r1 + margin)
    return Window(c0, r0, c1 - c0, r1 - r0)


def _rasterize(area: BaseGeometry, grid: Grid, window: Window) -> np.ndarray:
    return features.rasterize([area], out_shape=(window.height, window.width), transform=window_transform(window, grid.transform),
                              fill=0, default_value=1, dtype="uint8").astype(bool)


def _firesim() -> Path:
    exe = PROJECT_ROOT / "sim" / "build" / ("firesim.exe" if os.name == "nt" else "firesim")
    if not exe.exists():
        raise FileNotFoundError(f"{exe} absent : compiler le module de simulation (sim/build.bat)")
    return exe


def simulate(entry: dict, run_dir: Path) -> str:
    lat, lon = entry["point"]
    source, times, rows = meteo.load(lat, lon, datetime.fromisoformat(entry["depart"]), entry["duree_h"])
    meteo.write(run_dir / "meteo.csv", source, lat, lon, times, rows)
    subprocess.run([str(_firesim()), "run", "--ignition", f"{lat},{lon}", "--meteo", str(run_dir / "meteo.csv"),
                    "--start", entry["depart"], "--duration", f"{entry['duree_h']}h", "--moisture", entry["vegetation_vivante"],
                    "--landscape", str(PROCESSED_DIR / "sim"), "--out", str(run_dir / "sim")], check=True)
    return source


def _scores(arrival: np.ndarray, observed: np.ndarray, minutes: float, cell_ha: float) -> dict:
    simulated = arrival <= minutes
    common = int((simulated & observed).sum())
    n_sim, n_obs = int(simulated.sum()), int(observed.sum())
    return {
        "simule_ha": round(n_sim * cell_ha, 1),
        "commun_ha": round(common * cell_ha, 1),
        # Indice de Sørensen : 1 si les deux zones coïncident, 0 si elles ne se touchent pas
        "sorensen": round(2 * common / (n_sim + n_obs), 3),
        "part_du_reel_atteinte": round(common / n_obs, 3),
        "part_simulee_hors_du_reel": round((n_sim - common) / max(n_sim, 1), 3),
    }


def compare(fire_id: str, entry: dict, area: BaseGeometry, run_dir: Path, source: str) -> dict:
    grid = Grid.load()
    run = json.loads((run_dir / "sim" / "run.json").read_text(encoding="utf-8"))
    w = run["window"]
    oc0, or0, oc1, or1 = _cells(grid, area.bounds)
    window = _window(grid, min(oc0, w["col"]), min(or0, w["row"]), max(oc1, w["col"] + w["width"]),
                     max(or1, w["row"] + w["height"]), MARGIN_CELLS)
    observed = _rasterize(area, grid, window)
    arrival = np.full(observed.shape, np.inf, dtype="float32")
    sim = np.fromfile(run_dir / "sim" / "arrival_min.f32", dtype="<f4").reshape(w["height"], w["width"])
    r, c = w["row"] - window.row_off, w["col"] - window.col_off
    arrival[r : r + w["height"], c : c + w["width"]] = np.where(np.isnan(sim), np.inf, sim)

    cell_ha = grid.res**2 / 1e4
    duration_min = entry["duree_h"] * 60.0
    start = datetime.fromisoformat(entry["depart"])
    result = {"feu": fire_id, "nom": entry["nom"], "meteo": source, "observe_ha": round(observed.sum() * cell_ha, 1),
              "fin": {"heure_h": entry["duree_h"], **_scores(arrival, observed, duration_min, cell_ha)}}
    # Même surface que le feu réel : la forme est-elle juste, indépendamment de la vitesse ?
    reached = np.sort(arrival[np.isfinite(arrival)])
    n_obs = int(observed.sum())
    result["meme_surface"] = ({"heure_h": round(float(reached[n_obs - 1]) / 60, 2),
                               **_scores(arrival, observed, float(reached[n_obs - 1]), cell_ha)}
                              if reached.size >= n_obs else None)
    result["observations"] = []
    for when, hectares in (entry.get("observations") or {}).items():
        minutes = (datetime.fromisoformat(when) - start).total_seconds() / 60
        if 0 <= minutes <= duration_min:
            result["observations"].append({"heure": when, "annonce_ha": hectares,
                                           "simule_ha": round(float((arrival <= minutes).sum()) * cell_ha, 1)})
    _figure(entry, grid, window, arrival, observed, result, run_dir / "comparaison.png")
    (run_dir / "comparaison.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    return result


def _figure(entry: dict, grid: Grid, window: Window, arrival: np.ndarray, observed: np.ndarray, result: dict, out: Path) -> None:
    with rasterio.open(ELEVATION) as src:
        elev = src.read(1, window=window)
    with rasterio.open(FUEL) as src:
        fuel = src.read(1, window=window)
    background = _fuel_rgb(fuel) * (0.55 + 0.45 * hillshade(elev, grid.res)[..., None])
    hours = np.where(np.isfinite(arrival), arrival / 60, np.nan)

    fig, ax = plt.subplots(figsize=(11, 11 * elev.shape[0] / elev.shape[1] + 1.5))
    ax.imshow(background)
    im = ax.imshow(np.ma.masked_invalid(hours), cmap="inferno_r", alpha=0.8, vmin=0, vmax=entry["duree_h"])
    fig.colorbar(im, ax=ax, shrink=0.6, label="heure d'arrivée du feu simulé (h après le départ)")
    ax.contour(observed.astype(float), levels=[0.5], colors="cyan", linewidths=2)
    lat, lon = entry["point"]
    x, y = rasterio.warp.transform("EPSG:4326", grid.crs, [lon], [lat])
    ax.plot((x[0] - grid.left) / grid.res - window.col_off - 0.5, (grid.top - y[0]) / grid.res - window.row_off - 0.5,
            marker="*", color="cyan", markersize=16, markeredgecolor="black")
    km = 1000 / grid.res
    ax.plot([0.04 * elev.shape[1], 0.04 * elev.shape[1] + km], [0.96 * elev.shape[0]] * 2, color="white", lw=3)
    ax.text(0.04 * elev.shape[1] + km / 2, 0.94 * elev.shape[0], "1 km", color="white", ha="center", fontsize=9)

    end, same = result["fin"], result["meme_surface"]
    lines = [f"{entry['nom']}, départ le {datetime.fromisoformat(entry['depart']):%d/%m/%Y à %H h} — météo {result['meteo']}",
             f"Contour bleu : zone brûlée réelle (EFFIS), {result['observe_ha']:.0f} ha. "
             f"Simulé en {entry['duree_h']} h : {end['simule_ha']:.0f} ha, recouvrement (Sørensen) {end['sorensen']:.2f}"]
    if same:
        lines.append(f"À surface égale ({same['heure_h']:.1f} h) : recouvrement {same['sorensen']:.2f}, "
                     f"{same['part_du_reel_atteinte'] * 100:.0f} % de la zone réelle atteinte")
    ax.set_title("\n".join(lines), fontsize=10)
    ax.set_axis_off()
    fig.savefig(out, dpi=110, bbox_inches="tight")
    plt.close(fig)


def export_for_unreal(fire_id: str, entry: dict, area: BaseGeometry) -> Path:
    """Le feu et son contour observé sur la grille de simulation : 0 dehors, 1 brûlé, 2 bord (OUTLINE_CELLS)."""
    grid = Grid.load()
    window = _window(grid, *_cells(grid, area.bounds), 3)
    inside = _rasterize(area, grid, window)
    k = OUTLINE_CELLS
    padded = np.pad(inside, k)
    interior = inside.copy()
    for dy in range(-k, k + 1):
        for dx in range(-k, k + 1):
            interior &= padded[k + dy : k + dy + inside.shape[0], k + dx : k + dx + inside.shape[1]]
    mask = inside.astype("uint8")
    mask[inside & ~interior] = 2
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / f"{fire_id}_contour.u8").write_bytes(mask.tobytes())
    info = {
        "id": fire_id, "nom": entry["nom"], "nom_court": entry.get("nom_court", entry["nom"]), "depart": entry["depart"],
        "latitude": entry["point"][0],
        "longitude": entry["point"][1], "duree_h": entry["duree_h"], "vegetation_vivante": entry["vegetation_vivante"],
        "observe_ha": round(float(inside.sum()) * grid.res**2 / 1e4, 1),
        "contour": {"fichier": f"{fire_id}_contour.u8", "col": int(window.col_off), "row": int(window.row_off),
                    "width": int(window.width), "height": int(window.height), "source": "EFFIS (Copernicus)"},
        "observations": [{"heure": h, "ha": ha} for h, ha in (entry.get("observations") or {}).items()],
    }
    path = OUT_DIR / f"{fire_id}.json"
    path.write_text(json.dumps(info, indent=2, ensure_ascii=False), encoding="utf-8")
    return path


def replay(fire_id: str, entry: dict) -> dict:
    print(f"== {entry['nom']} ({fire_id})", flush=True)
    area = observed_area(fire_id, entry)
    print(f"  écrit : {export_for_unreal(fire_id, entry, area).name}")
    run_dir = RUNS_DIR / fire_id
    source = simulate(entry, run_dir)
    result = compare(fire_id, entry, area, run_dir, source)
    end, same = result["fin"], result["meme_surface"]
    print(f"  réel {result['observe_ha']:.0f} ha ; simulé {end['simule_ha']:.0f} ha en {entry['duree_h']} h, "
          f"recouvrement {end['sorensen']:.2f}")
    if same:
        print(f"  à surface égale ({same['heure_h']:.1f} h) : recouvrement {same['sorensen']:.2f}")
    for obs in result["observations"]:
        print(f"  {obs['heure']} : annoncé {obs['annonce_ha']} ha, simulé {obs['simule_ha']:.0f} ha")
    print(f"  écrit : {run_dir / 'comparaison.png'}")
    return result


def main(argv: list[str] | None = None) -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(prog="rejeu", description="Rejoue un feu réel et le compare à la zone brûlée observée.")
    parser.add_argument("feu", nargs="?", help="identifiant du feu dans le catalogue, ou « all »")
    args = parser.parse_args(argv)
    fires = catalogue()
    if not args.feu:
        for fire_id, entry in fires.items():
            print(f"{fire_id:16s} {entry['nom']}, départ le {entry['depart']}, {entry['duree_h']} h")
        return
    if args.feu != "all" and args.feu not in fires:
        sys.exit(f"feu inconnu : {args.feu} (voir {CATALOGUE.name})")
    for fire_id in fires if args.feu == "all" else [args.feu]:
        replay(fire_id, fires[fire_id])


if __name__ == "__main__":
    main()

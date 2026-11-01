"""Calibration du simulateur sur les feux réels du catalogue : chaque feu est rejoué avec une grille de réglages, et
on garde ceux qui collent le mieux aux zones brûlées observées (EFFIS).

    python -I -m firesim_pipeline.calibration

La lutte contre le feu réduit la surface finale des feux réels, et le simulateur ne la connaît pas encore. On juge donc :
- la forme : recouvrement (Sørensen) au moment où le feu simulé atteint la surface réelle, quelle que soit sa vitesse ;
- la vitesse : surfaces simulées / surfaces annoncées pendant le feu (presse, pompiers), en écart logarithmique.
Règle : la lutte ne peut que ralentir un feu, donc le simulateur doit aller au moins aussi vite que le moins combattu. On
prend le plus petit facteur de vitesse qui ne sous-estime aucun feu, et les réglages de meilleure forme moyenne. Une
validation croisée juge chaque feu avec les réglages calibrés sur les autres, feux que la calibration n'a donc pas vus.
Les feux dont la végétation mesurée est postérieure au feu (vegetation_apres_feu au catalogue) ne servent pas au choix :
ils sont seulement rapportés.
Résultats : runs/calibration/essais.csv (chaque essai) et synthese.json. Il faut avoir lancé firesim_pipeline.rejeu.
"""
import argparse
import csv
import json
import math
import statistics
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import datetime
from itertools import product
from pathlib import Path

import numpy as np
from rasterio.windows import Window
from shapely.geometry.base import BaseGeometry

from . import rejeu
from .config import PROCESSED_DIR, PROJECT_ROOT
from .grid import Grid

OUT_DIR = PROJECT_ROOT / "runs" / "calibration"
# Forêts à sous-bois (TU5) : le couvert des arbres freine le vent, ou pas, ou le sous-bois brûle comme du maquis.
# En dessous d'environ 40 % de couvert, BehavePlus ne compte plus l'abri des arbres : c'est « sans couvert ».
FORESTS = {
    "couvert 50 %": ["--canopy-cover", "0.5"],
    "sans couvert": ["--no-canopy"],
    "maquis": ["--fuel-swap", "TU5=SH5"],
}
WIND_FACTORS = (0.75, 1.0, 1.25, 1.5)
ROS_FACTORS = (0.2, 0.3, 0.45, 0.7, 1.0)
BEFORE = ("couvert 50 %", 1.0, 0.44)  # réglages actuels du simulateur
SLOWEST_ALLOWED = math.log(0.9)  # simulé / annoncé : les surfaces annoncées sont des arrondis

Setting = tuple[str, float, float]  # forêts, facteur de vent, facteur de vitesse


@dataclass(eq=False)
class Fire:
    id: str
    entry: dict
    area: BaseGeometry
    observed_cells: int
    duration_min: float
    observations: list[tuple[float, float]] = field(default_factory=list)  # (minutes après le départ, ha annoncés)


def _fires(grid: Grid) -> list[Fire]:
    fires = []
    for fire_id, entry in rejeu.catalogue().items():
        if not (rejeu.RUNS_DIR / fire_id / "meteo.csv").exists():
            sys.exit(f"météo absente pour {fire_id} : lancer d'abord firesim_pipeline.rejeu {fire_id}")
        area = rejeu.observed_area(fire_id, entry)
        window = rejeu._window(grid, *rejeu._cells(grid, area.bounds), 1)
        start = datetime.fromisoformat(entry["depart"])
        fire = Fire(fire_id, entry, area, int(rejeu._rasterize(area, grid, window).sum()), entry["duree_h"] * 60.0)
        for when, hectares in (entry.get("observations") or {}).items():
            minutes = (datetime.fromisoformat(when) - start).total_seconds() / 60
            if 0 < minutes <= fire.duration_min:
                fire.observations.append((minutes, hectares))
        fires.append(fire)
    return fires


def _score(fire: Fire, run_dir: Path, grid: Grid) -> dict:
    w = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))["window"]
    arrival = np.fromfile(run_dir / "arrival_min.f32", dtype="<f4").reshape(w["height"], w["width"])
    arrival = np.where(np.isnan(arrival), np.inf, arrival)
    observed = rejeu._rasterize(fire.area, grid, Window(w["col"], w["row"], w["width"], w["height"]))
    times = np.sort(arrival[np.isfinite(arrival)])
    cell_ha = grid.res**2 / 1e4

    def burned(minutes: float) -> int:
        return int(np.searchsorted(times, minutes, side="right"))

    def sorensen(minutes: float) -> float:
        return 2 * int((observed & (arrival <= minutes)).sum()) / (burned(minutes) + fire.observed_cells)

    end = sorensen(fire.duration_min)
    reached = times.size >= fire.observed_cells
    speed = [math.log(max(burned(m), 1) * cell_ha / ha) for m, ha in fire.observations]
    return {"forme": sorensen(times[fire.observed_cells - 1]) if reached else end, "fin": end,
            "vitesse": statistics.fmean(speed) if speed else None, "surface_ha": times.size * cell_ha}


class Trials:
    """Rejeux déjà calculés, par feu et par réglage."""

    def __init__(self, fires: list[Fire], grid: Grid, workers: int):
        self.fires, self.grid, self.workers = fires, grid, workers
        self.done: dict[tuple[str, Setting], dict] = {}
        self.firesim = rejeu._firesim()

    def _run(self, fire: Fire, setting: Setting) -> dict:
        forest, wind, ros = setting
        lat, lon = fire.entry["point"]
        with tempfile.TemporaryDirectory(dir=OUT_DIR) as tmp:
            subprocess.run([str(self.firesim), "run", "--ignition", f"{lat},{lon}",
                            "--meteo", str(rejeu.RUNS_DIR / fire.id / "meteo.csv"), "--start", fire.entry["depart"],
                            "--duration", f"{fire.entry['duree_h']}h", "--moisture", fire.entry["vegetation_vivante"],
                            "--landscape", str(PROCESSED_DIR / "sim"), "--out", tmp, *FORESTS[forest],
                            "--wind-factor", str(wind), "--ros-factor", str(ros)], check=True, capture_output=True)
            return _score(fire, Path(tmp), self.grid)

    def run(self, settings: list[Setting], fires: list[Fire] | None = None) -> None:
        todo = [(f, s) for s in settings for f in fires or self.fires if (f.id, s) not in self.done]
        with ThreadPoolExecutor(self.workers) as pool:
            for (fire, setting), score in zip(todo, pool.map(lambda job: self._run(*job), todo)):
                self.done[fire.id, setting] = score

    def summary(self, setting: Setting, fires: list[Fire]) -> dict:
        scores = [self.done[f.id, setting] for f in fires]
        speeds = [s["vitesse"] for s in scores if s["vitesse"] is not None]
        return {"forme": statistics.fmean(s["forme"] for s in scores), "fin": statistics.fmean(s["fin"] for s in scores),
                "vitesse": statistics.median(speeds)}


def _fast_enough(trials: Trials, setting: Setting, fires: list[Fire]) -> bool:
    speeds = (trials.done[f.id, setting]["vitesse"] for f in fires)
    return min(s for s in speeds if s is not None) >= SLOWEST_ALLOWED


def calibrate(trials: Trials, fires: list[Fire]) -> Setting:
    """Pour chaque réglage des forêts et du vent, le plus petit facteur de vitesse qui ne sous-estime aucun feu. On garde
    la meilleure forme, puis on affine ce facteur par dichotomie."""
    trials.run(list(product(FORESTS, WIND_FACTORS, ROS_FACTORS)), fires)
    candidates = []
    for forest, wind in product(FORESTS, WIND_FACTORS):
        enough = [r for r in ROS_FACTORS if _fast_enough(trials, (forest, wind, r), fires)]
        if enough:
            candidates.append((forest, wind, enough[0]))
    forest, wind, high = max(candidates, key=lambda s: trials.summary(s, fires)["forme"])
    low = max((r for r in ROS_FACTORS if r < high), default=None)
    while low is not None and high - low > 0.02:
        middle = round((high + low) / 2, 2)
        trials.run([(forest, wind, middle)], fires)
        if _fast_enough(trials, (forest, wind, middle), fires):
            high = middle
        else:
            low = middle
    return forest, wind, high


def _label(setting: Setting) -> str:
    forest, wind, ros = setting
    return f"forêts : {forest}, vent ×{wind:g}, vitesse ×{ros:g}"


def main(argv: list[str] | None = None) -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(prog="calibration", description="Calibre le simulateur sur les feux réels du catalogue.")
    parser.add_argument("--workers", type=int, default=10, help="simulations en parallèle [10]")
    args = parser.parse_args(argv)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    grid = Grid.load()
    fires = _fires(grid)
    judged = [f for f in fires if not f.entry.get("vegetation_apres_feu")]
    trials = Trials(fires, grid, args.workers)
    print(f"Grille : {len(FORESTS) * len(WIND_FACTORS) * len(ROS_FACTORS)} réglages × {len(judged)} feux", flush=True)

    chosen = calibrate(trials, judged)
    trials.run([BEFORE, chosen])
    print(f"\nMeilleurs réglages sur l'ensemble des feux : {_label(chosen)}")
    print(f"{'':20s}{'forme':>14s}{'fin':>14s}{'vitesse':>16s}")
    per_fire = {}
    for fire in fires:
        before, after = trials.done[fire.id, BEFORE], trials.done[fire.id, chosen]
        speed = "" if after["vitesse"] is None else f"×{math.exp(before['vitesse']):.2f} → ×{math.exp(after['vitesse']):.2f}"
        mark = "" if fire in judged else " (rapporté)"
        print(f"{fire.id:20s}{before['forme']:6.2f} → {after['forme']:.2f}{before['fin']:6.2f} → {after['fin']:.2f}  {speed}{mark}")
        per_fire[fire.id] = {"avant": before, "apres": after}
    totals = {"avant": trials.summary(BEFORE, judged), "apres": trials.summary(chosen, judged)}
    print(f"{'moyenne':20s}{totals['avant']['forme']:6.2f} → {totals['apres']['forme']:.2f}"
          f"{totals['avant']['fin']:6.2f} → {totals['apres']['fin']:.2f}  "
          f"×{math.exp(totals['avant']['vitesse']):.2f} → ×{math.exp(totals['apres']['vitesse']):.2f} (médiane)")

    print("\nValidation croisée : chaque feu jugé avec les réglages calibrés sans lui")
    cross = {}
    for fire in judged:
        setting = calibrate(trials, [f for f in judged if f is not fire])
        trials.run([setting], [fire])
        before, after = trials.done[fire.id, BEFORE], trials.done[fire.id, setting]
        cross[fire.id] = {"reglages": dict(zip(("forets", "vent", "vitesse"), setting)), "avant": before, "apres": after}
        print(f"{fire.id:20s}forme {before['forme']:.2f} → {after['forme']:.2f}, vitesse ×{math.exp(after['vitesse']):.2f}   "
              f"({_label(setting)})")
    cross_before = statistics.fmean(c["avant"]["forme"] for c in cross.values())
    cross_after = statistics.fmean(c["apres"]["forme"] for c in cross.values())
    print(f"{'moyenne':20s}forme {cross_before:.2f} → {cross_after:.2f}")
    adopt = chosen != BEFORE and cross_after > cross_before
    verdict = f"adopter {_label(chosen)}" if adopt else f"garder les réglages actuels ({_label(BEFORE)})"
    print(f"\nVerdict : {verdict}")

    with (OUT_DIR / "essais.csv").open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["feu", "forets", "facteur_vent", "facteur_vitesse", "forme", "fin", "vitesse_log", "surface_ha"])
        for (fire_id, (forest, wind, ros)), s in sorted(trials.done.items()):
            writer.writerow([fire_id, forest, wind, ros, round(s["forme"], 4), round(s["fin"], 4),
                             "" if s["vitesse"] is None else round(s["vitesse"], 4), round(s["surface_ha"], 1)])
    synthesis = {"reglages": dict(zip(("forets", "vent", "vitesse"), chosen)), "adopter": adopt, "verdict": verdict,
                 "feux": per_fire, "moyennes": totals,
                 "validation_croisee": {"forme_avant": cross_before, "forme_apres": cross_after, "feux": cross}}
    (OUT_DIR / "synthese.json").write_text(json.dumps(synthesis, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nÉcrit : {OUT_DIR / 'essais.csv'}, {OUT_DIR / 'synthese.json'}")


if __name__ == "__main__":
    main()

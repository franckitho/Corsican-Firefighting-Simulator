"""Météo réelle au point de départ d'un feu, heure par heure, pour l'outil firesim (option --meteo).

    python -I -m firesim_pipeline.meteo --at 42.62,9.355 --start 2025-07-20T13:00 --duration 24h --out runs/meteo.csv
    sim/build/firesim run --ignition 42.62,9.355 --meteo runs/meteo.csv --start 2025-07-20T13:00 --duration 24h ...

Source : Open-Meteo, en heure locale de la Corse.
- Archives des prévisions de Météo-France, du 13 novembre 2022 jusqu'aux prévisions des jours à venir : pour chaque
  grandeur, AROME 1,3 km, sinon AROME 2,5 km, sinon ARPEGE 10 km.
- Avant, ou s'il manque des heures : la réanalyse ERA5 (30 km, depuis 1940, jusqu'à 5 jours avant aujourd'hui).
Les relevés commencent 14 jours avant le feu : firesim en calcule l'humidité des végétaux morts heure par heure.
Le projet Unreal fait la même chose de son côté (Plugins/FireSim/Source/FireSim/Private/RealWeather.cpp).
"""
import argparse
import csv
import sys
from datetime import datetime, timedelta
from pathlib import Path

import requests

SPINUP_DAYS = 14
TIMEZONE = "Europe/Paris"
# Colonne du CSV de firesim -> grandeur d'Open-Meteo
VARIABLES = {
    "vent_kmh": "wind_speed_10m",
    "vent_de_deg": "wind_direction_10m",
    "temperature_c": "temperature_2m",
    "humidite_pct": "relative_humidity_2m",
    "rayonnement_wm2": "shortwave_radiation",
    "pluie_mm": "precipitation",
}
SOURCES = [
    ("Météo-France AROME / ARPEGE (archives des prévisions)",
     "https://historical-forecast-api.open-meteo.com/v1/forecast",
     ["meteofrance_arome_france_hd", "meteofrance_arome_france", "meteofrance_arpege_europe"]),
    ("ERA5 (réanalyse, 30 km)", "https://archive-api.open-meteo.com/v1/archive", []),
]


def _fetch(url: str, models: list[str], lat: float, lon: float, first: datetime, last: datetime) -> tuple[list[str], list[list]]:
    params = {
        "latitude": lat, "longitude": lon, "timezone": TIMEZONE, "hourly": ",".join(VARIABLES.values()),
        "start_date": first.date().isoformat(), "end_date": last.date().isoformat(),
    }
    if models:
        params["models"] = ",".join(models)
    response = requests.get(url, params=params, timeout=60)
    if response.status_code != 200:
        raise RuntimeError(f"{url} : {response.status_code} {response.text[:200]}")
    hourly = response.json()["hourly"]
    keys = [[f"{v}_{m}" for m in models] if models else [v] for v in VARIABLES.values()]
    rows = []
    for i in range(len(hourly["time"])):
        # Pour chaque grandeur, le premier modèle qui a une valeur
        rows.append([next((hourly[k][i] for k in candidates if hourly[k][i] is not None), None) for candidates in keys])
    return hourly["time"], rows


def _usable(times: list[str], rows: list[list], start: str) -> tuple[list[str], list[list]] | None:
    """Relevés complets jusqu'au départ du feu, puis jusqu'au premier trou (la dernière heure connue vaut ensuite)."""
    if start not in times:
        return None
    ignition = times.index(start)
    if any(None in row for row in rows[: ignition + 1]):
        return None
    end = next((i for i in range(ignition + 1, len(rows)) if None in rows[i]), len(rows))
    return times[:end], rows[:end]


def load(lat: float, lon: float, start: datetime, hours: int) -> tuple[str, list[str], list[list]]:
    first, last = start - timedelta(days=SPINUP_DAYS), start + timedelta(hours=hours)
    key = start.strftime("%Y-%m-%dT%H:00")
    for name, url, models in SOURCES:
        usable = _usable(*_fetch(url, models, lat, lon, first, last), key)
        if usable:
            return name, *usable
    raise RuntimeError(f"pas de météo complète pour le {start:%d/%m/%Y à %H h} ({SPINUP_DAYS} jours avant compris)")


def write(path: Path, source: str, lat: float, lon: float, times: list[str], rows: list[list]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write(f"# Météo réelle à {lat:.4f}, {lon:.4f}, heure locale. Source : {source}, via Open-Meteo.\n")
        writer = csv.writer(f)
        writer.writerow(["date_heure", *VARIABLES])
        for time, row in zip(times, rows):
            writer.writerow([time, *row])


def _hours(text: str) -> int:
    return int(float(text.removesuffix("h"))) if text.endswith("h") else int(float(text) / 60)


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--at", required=True, help="point de départ du feu : LAT,LON")
    parser.add_argument("--start", required=True, help="heure locale du départ : AAAA-MM-JJTHH:00")
    parser.add_argument("--duration", default="24h", help="durée à couvrir après le départ (h) [24h]")
    parser.add_argument("--out", required=True, type=Path, help="fichier CSV à écrire")
    args = parser.parse_args()
    lat, lon = (float(v) for v in args.at.split(","))
    start = datetime.fromisoformat(args.start)
    source, times, rows = load(lat, lon, start, _hours(args.duration))
    write(args.out, source, lat, lon, times, rows)
    at_start = dict(zip(VARIABLES, rows[times.index(start.strftime("%Y-%m-%dT%H:00"))]))
    print(f"Source : {source}")
    print(f"{len(times)} heures, du {times[0]} au {times[-1]}")
    print(f"Au départ : vent {at_start['vent_kmh']:.0f} km/h venant de {at_start['vent_de_deg']:.0f}°, "
          f"{at_start['temperature_c']:.0f} °C, {at_start['humidite_pct']:.0f} % d'humidité")
    print(f"Fichier : {args.out}")


if __name__ == "__main__":
    main()

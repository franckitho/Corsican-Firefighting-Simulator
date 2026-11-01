"""Étape 3 : eau et coupures linéaires (rivières, routes) d'après la BD TOPO."""
import numpy as np
import yaml

from .config import CONFIG_DIR, PROCESSED_DIR
from .grid import Grid, write
from .vectors import burn, find, read_all

WATER = PROCESSED_DIR / "water.tif"
BARRIER_WIDTH = PROCESSED_DIR / "barrier_width.tif"

IN_SERVICE = "etat_de_l_objet = 'En service'"


def _river_widths(gpkgs, crs, cfg) -> list:
    rivers = read_all(
        gpkgs, crs, layer="troncon_hydrographique",
        columns=["classe_de_largeur", "persistance", "position_par_rapport_au_sol", "fictif"],
        where=IN_SERVICE,
    )
    # Pas les tronçons fictifs (traversée d'un lac) ni souterrains
    rivers = rivers[~rivers["fictif"].fillna(False).astype(bool) & (rivers["position_par_rapport_au_sol"] != "-1")]
    width = rivers["classe_de_largeur"].map(cfg["widths"]).fillna(cfg["default_width"]).astype(float)
    width = np.where(rivers["persistance"] == "Intermittent", width * cfg["intermittent_factor"], width)
    print(f"  rivières : {len(rivers)} tronçons", flush=True)
    return list(zip(rivers.geometry, width))


def _road_widths(gpkgs, crs, cfg) -> list:
    roads = read_all(
        gpkgs, crs, layer="troncon_de_route",
        columns=["nature", "largeur_de_chaussee", "position_par_rapport_au_sol", "fictif"],
        where=IN_SERVICE,
    )
    # Ponts et tunnels ne coupent pas la végétation au sol
    keep = (
        ~roads["fictif"].fillna(False).astype(bool)
        & ~roads["position_par_rapport_au_sol"].isin(["1", "2", "-1", "-2"])
        & ~roads["nature"].isin(cfg["excluded"])
    )
    roads = roads[keep]
    default = roads["nature"].map(cfg["widths"]).fillna(min(cfg["widths"].values()))
    width = roads["largeur_de_chaussee"].where(roads["largeur_de_chaussee"] > 0, default).astype(float)
    print(f"  routes et chemins : {len(roads)} tronçons", flush=True)
    return list(zip(roads.geometry, width))


def run() -> None:
    grid = Grid.load()
    cfg = yaml.safe_load((CONFIG_DIR / "barriers.yaml").read_text(encoding="utf-8"))
    gpkgs = find("bdtopo", "*.gpkg")

    surfaces = read_all(gpkgs, grid.crs, layer="surface_hydrographique", columns=["nature"], where=IN_SERVICE)
    surfaces = surfaces[~surfaces["nature"].isin(cfg["water_excluded_natures"])]
    water = burn(grid, ((g, 1) for g in surfaces.geometry), "uint8")
    print(f"  surfaces d'eau : {len(surfaces)}", flush=True)

    lines = _river_widths(gpkgs, grid.crs, cfg["rivers"]) + _road_widths(gpkgs, grid.crs, cfg["roads"])
    # Tri croissant : là où plusieurs coupures se croisent, la plus large l'emporte.
    lines.sort(key=lambda gw: gw[1])
    width = burn(grid, ((g, int(min(round(w), 255))) for g, w in lines), "uint8", all_touched=True)

    write(grid, WATER, water, None, description="1 = eau libre (BD TOPO)")
    write(grid, BARRIER_WIDTH, width, None, units="m", description="largeur de la coupure la plus large de la cellule")

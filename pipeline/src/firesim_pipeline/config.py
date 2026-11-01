"""Chemins et constantes partagés par toutes les étapes du pipeline."""
import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
CONFIG_DIR = PROJECT_ROOT / "pipeline" / "config"

# Les données pèsent plusieurs Go : FIRESIM_DATA_DIR permet de les mettre sur un autre disque.
DATA_DIR = Path(os.environ.get("FIRESIM_DATA_DIR", PROJECT_ROOT / "data"))
RAW_DIR = DATA_DIR / "raw"
PROCESSED_DIR = DATA_DIR / "processed"

CRS = "EPSG:2154"  # Lambert 93, système natif des données IGN
DEM_RES = 5.0      # pas du RGE ALTI (m)
SIM_RES = 25.0     # pas de la grille de simulation (m)

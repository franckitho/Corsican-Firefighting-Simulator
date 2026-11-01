"""Étape 3 bis : hauteur de la végétation, d'après la carte mondiale de hauteur du couvert à 1 m de Meta et du WRI
(images de 2018-2019).

La BD Forêt range en « forêt fermée » aussi bien les futaies, où le feu court sous les arbres à l'abri du vent, que le
maquis arboré (chêne vert, arbousier de quelques mètres), qui brûle comme du maquis. La hauteur mesurée les sépare.
Produit, sur la grille de 25 m :
- vegetation_height.tif : hauteur dominante (m), 90e centile des hauteurs moyennes à 5 m dans la cellule ;
- tree_cover.tif : part de la cellule couverte d'arbres d'au moins 5 m (%), au sens de la BD Forêt et de la FAO.
Les zones brûlées après les images (feux de 2019 et après) y ont encore leur végétation d'avant.
"""
import numpy as np
import rasterio
from rasterio.enums import Resampling
from rasterio.errors import WindowError
from rasterio.vrt import WarpedVRT
from rasterio.warp import transform_bounds
from rasterio.windows import Window, from_bounds

from .config import PROCESSED_DIR
from .grid import Grid, write
from .vectors import find

HEIGHT = PROCESSED_DIR / "vegetation_height.tif"
TREE_COVER = PROCESSED_DIR / "tree_cover.tif"
FINE_RES = 5.0      # pas intermédiaire (m) : assez fin pour voir les houppiers, assez gros pour tenir en mémoire
TREE_HEIGHT_M = 5   # hauteur à partir de laquelle on compte un arbre
DOMINANT = 0.9      # centile de la hauteur dominante
ROWS_PER_CHUNK = 500


def _fine_heights(fine: Grid) -> np.ndarray:
    """Hauteur moyenne (m) sur la grille de 5 m, dalle par dalle."""
    out = np.zeros((fine.height, fine.width), dtype="uint8")
    for tif in find("canopee", "*.tif"):
        with rasterio.open(tif) as src:
            try:
                window = from_bounds(*transform_bounds(src.crs, fine.crs, *src.bounds), fine.transform)
                window = window.round_offsets().round_lengths().intersection(Window(0, 0, fine.width, fine.height))
            except WindowError:
                continue  # dalle hors de la grille
            with WarpedVRT(src, crs=fine.crs, transform=fine.transform, width=fine.width, height=fine.height,
                           resampling=Resampling.average, warp_mem_limit=1024, warp_extras={"NUM_THREADS": "ALL_CPUS"}) as vrt:
                data = vrt.read(1, window=window)
        rows, cols = window.toslices()
        np.maximum(out[rows, cols], data, out=out[rows, cols])
        print(f"  hauteur de la végétation : {tif.name}", flush=True)
    return out


def run() -> None:
    grid = Grid.load()
    fine = grid.scaled(FINE_RES)
    heights = _fine_heights(fine)
    f = round(grid.res / FINE_RES)
    k = round(DOMINANT * (f * f - 1))
    height = np.zeros((grid.height, grid.width), dtype="uint8")
    cover = np.zeros((grid.height, grid.width), dtype="uint8")
    for r0 in range(0, grid.height, ROWS_PER_CHUNK):
        r1 = min(r0 + ROWS_PER_CHUNK, grid.height)
        blocks = heights[r0 * f : r1 * f].reshape(r1 - r0, f, grid.width, f).transpose(0, 2, 1, 3).reshape(r1 - r0, grid.width, f * f)
        height[r0:r1] = np.partition(blocks, k, axis=2)[..., k]
        cover[r0:r1] = 100 * np.count_nonzero(blocks >= TREE_HEIGHT_M, axis=2) // (f * f)
    write(grid, HEIGHT, height, None, description="hauteur dominante de la végétation (m), Meta et WRI")
    write(grid, TREE_COVER, cover, None, description=f"couvert d'arbres d'au moins {TREE_HEIGHT_M} m (%), Meta et WRI")

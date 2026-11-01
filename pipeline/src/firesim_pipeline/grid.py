"""Grille commune : toutes les couches de simulation partagent exactement la même."""
import json
from dataclasses import asdict, dataclass, replace
from pathlib import Path

import numpy as np
import rasterio
from affine import Affine

from .config import CRS, PROCESSED_DIR

GRID_FILE = PROCESSED_DIR / "grid.json"


@dataclass(frozen=True)
class Grid:
    left: float
    top: float
    res: float
    width: int
    height: int
    crs: str = CRS

    @property
    def transform(self) -> Affine:
        return Affine(self.res, 0.0, self.left, 0.0, -self.res, self.top)

    @property
    def bounds(self) -> tuple[float, float, float, float]:
        return (self.left, self.top - self.height * self.res, self.left + self.width * self.res, self.top)

    def scaled(self, res: float) -> "Grid":
        """Même emprise à un autre pas (qui doit diviser le pas courant)."""
        factor = self.res / res
        if abs(factor - round(factor)) > 1e-9:
            raise ValueError(f"le pas {res} ne divise pas {self.res}")
        f = round(factor)
        return replace(self, res=res, width=self.width * f, height=self.height * f)

    def profile(self, dtype: str, nodata: float | int | None) -> dict:
        return {
            "driver": "GTiff",
            "dtype": dtype,
            "nodata": nodata,
            "width": self.width,
            "height": self.height,
            "count": 1,
            "crs": self.crs,
            "transform": self.transform,
            "tiled": True,
            "blockxsize": 512,
            "blockysize": 512,
            "compress": "deflate",
            "predictor": 3 if np.dtype(dtype).kind == "f" else 2,
            "BIGTIFF": "IF_SAFER",
        }

    def save(self, path: Path = GRID_FILE) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(asdict(self), indent=2))

    @classmethod
    def load(cls, path: Path = GRID_FILE) -> "Grid":
        if not path.exists():
            raise FileNotFoundError(f"{path} absent : lance d'abord l'étape « dem »")
        return cls(**json.loads(path.read_text()))


def write(grid: Grid, path: Path, data: np.ndarray, nodata: float | int | None, **tags: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with rasterio.open(path, "w", **grid.profile(data.dtype.name, nodata)) as dst:
        dst.write(data, 1)
        if tags:
            dst.update_tags(**tags)
    print(f"  écrit : {path.name}", flush=True)


def read(path: Path) -> np.ndarray:
    with rasterio.open(path) as src:
        return src.read(1)

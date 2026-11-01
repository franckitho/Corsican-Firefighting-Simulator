"""Catalogue des données brutes à télécharger."""
from dataclasses import dataclass

GPF = "https://data.geopf.fr/telechargement/download"
WORLDCOVER = "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map"
CANOPY = "https://dataforgood-fb-data.s3.amazonaws.com/forests/v1/alsgedi_global_v6_float/chm"


@dataclass(frozen=True)
class Source:
    group: str              # sous-dossier de data/raw/
    url: str
    size: int               # taille annoncée par le serveur (octets), pour contrôle
    md5_url: str | None = None
    extract: bool = False


def _ign(group: str, product: str, edition: str, size: int, md5: bool = False) -> Source:
    base = f"{GPF}/{product}/{edition}/{edition}"
    return Source(group, f"{base}.7z", size, f"{base}.md5" if md5 else None, extract=True)


def _worldcover(tile: str, size: int) -> Source:
    return Source("worldcover", f"{WORLDCOVER}/ESA_WorldCover_10m_2021_v200_{tile}_Map.tif", size)


def _canopy(quadkey: str, size: int) -> Source:
    return Source("canopee", f"{CANOPY}/{quadkey}.tif", size)


SOURCES = [
    # Relief à 5 m, altitudes IGN78C (nivellement de la Corse)
    _ign("rgealti", "RGEALTI", "RGEALTI_2-0_5M_ASC_LAMB93-IGN78C_D02A_2020-04-16", 418922024, md5=True),
    _ign("rgealti", "RGEALTI", "RGEALTI_2-0_5M_ASC_LAMB93-IGN78C_D02B_2020-04-16", 473478042, md5=True),
    # Végétation : types de formations forestières et landes (maquis)
    _ign("bdforet", "BDFORET", "BDFORET_2-0__SHP_LAMB93_D02A_2017-05-10", 51523932),
    _ign("bdforet", "BDFORET", "BDFORET_2-0__SHP_LAMB93_D02B_2016-02-16", 69329533),
    # Rivières, plans d'eau, routes
    _ign("bdtopo", "BDTOPO", "BDTOPO_3-5_TOUSTHEMES_GPKG_LAMB93_D02A_2026-09-15", 123746647),
    _ign("bdtopo", "BDTOPO", "BDTOPO_3-5_TOUSTHEMES_GPKG_LAMB93_D02B_2026-09-15", 145506564),
    # Occupation du sol à 10 m, pour tout ce que la BD Forêt ne couvre pas
    _worldcover("N39E006", 23426377),
    _worldcover("N39E009", 26277576),
    _worldcover("N42E006", 51160212),
    _worldcover("N42E009", 72420601),
    # Hauteur de la végétation à 1 m (Meta et WRI, images de 2018-2019), par dalle de la grille mondiale
    # (quadkey de niveau 9) : sépare les vraies futaies du maquis arboré, tous deux « forêt » pour la BD Forêt.
    _canopy("120223300", 59465674),
    _canopy("120223301", 126756088),
    _canopy("120223302", 464791952),
    _canopy("120223303", 411191694),
    _canopy("120223320", 401318943),
    _canopy("120223321", 231877327),
    _canopy("120223322", 78227673),
    _canopy("120223323", 173252533),
]

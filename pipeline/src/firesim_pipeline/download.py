"""Étape 1 : téléchargement et décompression des données brutes."""
import hashlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path, PurePosixPath
from urllib.parse import urlparse

import py7zr
import requests

from .config import RAW_DIR
from .sources import SOURCES, Source

CHUNK = 1 << 20


def _filename(url: str) -> str:
    return PurePosixPath(urlparse(url).path).name


def _md5(path: Path) -> str:
    h = hashlib.md5()
    with path.open("rb") as f:
        while block := f.read(CHUNK * 8):
            h.update(block)
    return h.hexdigest()


def fetch(source: Source) -> Path:
    dest_dir = RAW_DIR / source.group
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / _filename(source.url)
    if dest.exists() and dest.stat().st_size == source.size:
        print(f"  déjà là : {dest.name}")
        return dest

    # Reprise d'un téléchargement interrompu
    part = dest.with_name(dest.name + ".part")
    done = part.stat().st_size if part.exists() else 0
    headers = {"Range": f"bytes={done}-"} if done else {}
    with requests.get(source.url, headers=headers, stream=True, timeout=60) as r:
        r.raise_for_status()
        if done and r.status_code != 206:
            done = 0  # le serveur ignore la reprise : on repart de zéro
        with part.open("ab" if done else "wb") as f:
            next_report = done
            for block in r.iter_content(CHUNK):
                f.write(block)
                done += len(block)
                if done >= next_report:
                    print(f"  {dest.name} : {done / 1e6:.0f} / {source.size / 1e6:.0f} Mo", flush=True)
                    next_report += 100 * CHUNK

    if part.stat().st_size != source.size:
        raise RuntimeError(f"{dest.name} : taille {part.stat().st_size} au lieu de {source.size}")
    if source.md5_url:
        expected = requests.get(source.md5_url, timeout=60).text.split()[0].lower()
        if _md5(part) != expected:
            raise RuntimeError(f"{dest.name} : somme MD5 incorrecte")
    part.replace(dest)
    print(f"  ok : {dest.name}")
    return dest


def extract(archive: Path) -> Path:
    out_dir = archive.with_suffix("")
    marker = out_dir / ".extracted"
    if marker.exists():
        return out_dir
    with py7zr.SevenZipFile(archive) as z:
        for name in z.getnames():
            p = PurePosixPath(name.replace("\\", "/"))
            if p.is_absolute() or ".." in p.parts:
                raise RuntimeError(f"{archive.name} : chemin suspect dans l'archive : {name}")
        print(f"  décompression de {archive.name}…", flush=True)
        z.extractall(out_dir)
    marker.touch()
    return out_dir


def run() -> None:
    # Téléchargements en parallèle (le débit est souvent limité par connexion), puis décompression.
    with ThreadPoolExecutor(max_workers=4) as pool:
        paths = list(pool.map(fetch, SOURCES))
    for source, path in zip(SOURCES, paths):
        if source.extract:
            extract(path)

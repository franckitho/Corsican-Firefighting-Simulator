"""Étape « ion » : envoi du relief 5 m sur Cesium ion, pour le terrain 3D d'Unreal.

Passe par l'API REST d'ion avec un jeton d'accès plutôt que par le site : variable d'environnement
CESIUM_ION_TOKEN, jeton créé sur https://ion.cesium.com/tokens avec les portées assets:read et assets:write.
Déroulé : création de l'asset, envoi du fichier dans le stockage S3 désigné par ion (identifiants valables
12 h), signal de fin d'envoi, puis suivi de la fabrication des tuiles.

L'identifiant de l'asset est gardé dans terrain/ion_asset.json : relancer l'étape reprend le suivi sans
renvoyer le fichier. Pour le renvoyer, supprimer ce fichier (et l'ancien asset sur ion).
"""
import json
import os
import threading
import time

import boto3
import requests
from boto3.s3.transfer import TransferConfig

from .dem import DEM_5M

API = "https://api.cesium.com/v1"
ASSET_FILE = DEM_5M.parent / "ion_asset.json"
CESIUM_WORLD_TERRAIN = 1  # asset présent sur tous les comptes ion
POLL_S = 30

ASSET = {
    "name": "Corse - relief 5 m",
    "description": "IGN RGE ALTI 2.0 (5 m, altitudes IGN78C), moyenne de la Corse. Projet firesim-corse.",
    "attribution": "IGN RGE ALTI 2.0 — Licence Ouverte Etalab 2.0",
    "type": "TERRAIN",
    "options": {
        "sourceType": "RASTER_TERRAIN",
        # Altitudes du fichier par rapport au niveau de la mer (ion les ramène à l'ellipsoïde via EGM96)
        "heightReference": "MEAN_SEA_LEVEL",
        # Hors de l'île et dans les trous (la mer), le terrain mondial de Cesium prend le relais
        "baseTerrainId": CESIUM_WORLD_TERRAIN,
    },
}


def _session() -> requests.Session:
    token = os.environ.get("CESIUM_ION_TOKEN")
    if not token:
        raise SystemExit("CESIUM_ION_TOKEN n'est pas défini : créer un jeton sur https://ion.cesium.com/tokens "
                         "(portées assets:read et assets:write)")
    session = requests.Session()
    session.headers["Authorization"] = f"Bearer {token}"
    return session


def _json(response: requests.Response) -> dict:
    if not response.ok:
        raise SystemExit(f"ion a répondu {response.status_code} ({response.request.method} {response.url}) : "
                         f"{response.text[:500]}")
    return response.json() if response.content else {}


class _Progress:
    """Affiche l'avancement de l'envoi tous les 5 %."""

    def __init__(self, total: int):
        self.total, self.sent, self.shown, self.start = total, 0, -5, time.monotonic()
        self.lock = threading.Lock()

    def __call__(self, chunk: int) -> None:
        with self.lock:
            self.sent += chunk
            percent = 100 * self.sent // self.total
            if percent >= self.shown + 5:
                self.shown = percent
                rate = self.sent / 1e6 / max(time.monotonic() - self.start, 1e-3)
                print(f"  envoi : {percent:3d} % ({rate:.1f} Mo/s)", flush=True)


def upload(session: requests.Session) -> int:
    created = _json(session.post(f"{API}/assets", json=ASSET))
    asset_id = created["assetMetadata"]["id"]
    ASSET_FILE.write_text(json.dumps({"asset_id": asset_id, "name": ASSET["name"]}, indent=2))
    print(f"  asset créé : {asset_id}", flush=True)

    loc = created["uploadLocation"]
    s3 = boto3.client("s3", region_name="us-east-1", endpoint_url=loc.get("endpoint"),
                      aws_access_key_id=loc["accessKey"], aws_secret_access_key=loc["secretAccessKey"],
                      aws_session_token=loc["sessionToken"])
    size = DEM_5M.stat().st_size
    print(f"  envoi de {DEM_5M.name} ({size / 1e9:.2f} Go)", flush=True)
    # Envoi en morceaux de 64 Mo, 4 à la fois ; un morceau raté est renvoyé seul
    config = TransferConfig(multipart_chunksize=64 * 2**20, max_concurrency=4)
    s3.upload_file(str(DEM_5M), loc["bucket"], loc["prefix"] + DEM_5M.name, Config=config,
                   Callback=_Progress(size))

    done = created["onComplete"]
    _json(session.request(done["method"], done["url"], json=done.get("fields", {})))
    print("  envoi terminé, ion fabrique les tuiles", flush=True)
    return asset_id


def wait(session: requests.Session, asset_id: int) -> dict:
    shown = None
    while True:
        meta = _json(session.get(f"{API}/assets/{asset_id}"))
        state = (meta["status"], meta.get("percentComplete"))
        if state != shown:
            print(f"  ion : {meta['status']} {meta.get('percentComplete', 0)} %", flush=True)
            shown = state
        if meta["status"] in ("COMPLETE", "ERROR", "DATA_ERROR"):
            return meta
        if meta["status"] == "AWAITING_FILES":
            raise SystemExit(f"l'asset {asset_id} attend toujours son fichier (envoi interrompu) : le supprimer sur "
                             f"ion, supprimer {ASSET_FILE} et relancer")
        time.sleep(POLL_S)


def run() -> None:
    session = _session()
    if ASSET_FILE.exists():
        asset_id = json.loads(ASSET_FILE.read_text())["asset_id"]
        print(f"  asset déjà créé : {asset_id} (supprimer {ASSET_FILE.name} pour renvoyer le fichier)", flush=True)
    else:
        asset_id = upload(session)
    meta = wait(session, asset_id)
    if meta["status"] != "COMPLETE":
        raise SystemExit(f"échec de la fabrication des tuiles ({meta['status']}) : voir l'asset {asset_id} sur ion")
    print(f"  terrain prêt : asset {asset_id}, à mettre dans Cesium for Unreal (Cesium World Terrain → Ion Asset ID)",
          flush=True)

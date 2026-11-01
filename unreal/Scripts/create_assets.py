"""Crée les assets du simulateur dans le projet Unreal, sans ouvrir l'éditeur :
- /FireSim/M_FireOverlay : matériau du calque de feu, projeté sur le terrain par un décalque ;
- /FireSim/M_FireAircraft et M_FireDrop : matériaux des avions et des gouttes de leurs largages ;
- /Game/Maps/Corse : carte avec le simulateur, le terrain Cesium, l'imagerie (Bing et IGN), le soleil et la caméra.

UnrealEditor-Cmd.exe <projet>/FireSimCorse.uproject -run=pythonscript -script=<projet>/Scripts/create_assets.py
Relancer le script recrée le matériau et complète la carte sans dupliquer ce qui existe.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firesim_assets  # noqa: E402

# Sans éditeur, le registre des assets n'a pas encore parcouru les dossiers : on le force, sinon les assets
# existants paraissent absents.
unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([firesim_assets.MATERIAL_DIR, "/Game"], True)
firesim_assets.create_overlay_material()
firesim_assets.create_aircraft_material()
firesim_assets.create_drop_material()
firesim_assets.create_map()

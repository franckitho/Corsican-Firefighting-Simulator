"""Au démarrage de l'éditeur : active le pilotage par scripts (Scripts/pilotage.py), utilisé par Claude Code.

Supprimer ce fichier pour ne plus le lancer automatiquement.
"""
import os
import runpy

import unreal

runpy.run_path(os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), "Scripts", "pilotage.py"))

"""Pilotage de l'éditeur ouvert par des scripts déposés dans Saved/Pilotage (utilisé par Claude Code).

Lancé au démarrage de l'éditeur par Content/Python/init_unreal.py ; à la main, dans la console (champ « Cmd ») :
    py "<dépôt>/unreal/Scripts/pilotage.py"

Toutes les demi-secondes, chaque fichier Saved/Pilotage/<nom>.py est exécuté dans l'éditeur, puis supprimé ;
ce qu'il affiche (ou l'erreur) est écrit dans Saved/Pilotage/<nom>.out. Relancer la commande remplace l'ancien
pilotage au lieu d'en ajouter un second.
"""
import builtins
import contextlib
import io
import os
import traceback

import unreal

INBOX = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "Pilotage")
PERIOD_S = 0.5


class _Pilotage:
    def __init__(self):
        os.makedirs(INBOX, exist_ok=True)
        self.elapsed = 0.0
        self.handle = unreal.register_slate_post_tick_callback(self.tick)

    def tick(self, delta_seconds):
        self.elapsed += delta_seconds
        if self.elapsed < PERIOD_S:
            return
        self.elapsed = 0.0
        for name in sorted(os.listdir(INBOX)):
            if name.endswith(".py"):
                self.run(os.path.join(INBOX, name))

    @staticmethod
    def run(path):
        output = io.StringIO()
        try:
            with open(path, encoding="utf-8") as f:
                code = f.read()
            os.remove(path)
            with contextlib.redirect_stdout(output):
                exec(compile(code, path, "exec"), {"unreal": unreal, "__name__": "__pilotage__"})
        except Exception:
            output.write(traceback.format_exc())
        result = path[:-3] + ".out"
        with open(result + ".tmp", "w", encoding="utf-8") as f:
            f.write(output.getvalue())
        os.replace(result + ".tmp", result)

    def stop(self):
        unreal.unregister_slate_post_tick_callback(self.handle)


if getattr(builtins, "_firesim_pilotage", None):
    builtins._firesim_pilotage.stop()
builtins._firesim_pilotage = _Pilotage()
unreal.log(f"FireSim : pilotage actif, scripts lus dans {INBOX}")

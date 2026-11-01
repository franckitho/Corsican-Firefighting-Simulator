"""Point d'entrée : firesim-pipeline <étape>."""
import argparse
import importlib
import sys

# Dans l'ordre d'exécution : chaque étape lit ce que les précédentes ont produit.
STEPS = {
    "download": "firesim_pipeline.download",
    "dem": "firesim_pipeline.dem",
    "barriers": "firesim_pipeline.barriers",
    "canopy": "firesim_pipeline.canopy",
    "fuel": "firesim_pipeline.fuel",
    "export": "firesim_pipeline.export",
    "quicklook": "firesim_pipeline.quicklook",
}
# Étapes longues ou nécessitant un logiciel externe : lancées seulement à la demande, pas par « all ».
OPTIONAL_STEPS = {
    "wind": "firesim_pipeline.windatlas",  # WindNinja, plusieurs minutes par direction
    "ion": "firesim_pipeline.ion",  # envoi du relief 5 m sur Cesium ion (jeton CESIUM_ION_TOKEN)
}


def main(argv: list[str] | None = None) -> None:
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")  # console Windows en cp1252 par défaut
    parser = argparse.ArgumentParser(
        prog="firesim-pipeline",
        description="Prépare les couches de la Corse pour la simulation de feux.",
    )
    parser.add_argument("step", choices=[*STEPS, *OPTIONAL_STEPS, "all"])
    args = parser.parse_args(argv)
    modules = STEPS | OPTIONAL_STEPS
    for step in STEPS if args.step == "all" else [args.step]:
        print(f"== {step}", flush=True)
        importlib.import_module(modules[step]).run()


if __name__ == "__main__":
    main()

"""Assets du simulateur : matériau du calque de feu et carte Corse. Utilisé par create_assets.py (sans éditeur)
et par le pilotage de l'éditeur ouvert."""
import os

import unreal

MATERIAL_DIR = "/FireSim"
MATERIAL_NAME = "M_FireOverlay"
MAP_PATH = "/Game/Maps/Corse"

mel = unreal.MaterialEditingLibrary
assets = unreal.EditorAssetLibrary


def node(material, cls, x, y, **properties):
    expression = mel.create_material_expression(material, cls, x, y)
    for name, value in properties.items():
        expression.set_editor_property(name, value)
    return expression


def connect(source, output, target, input_name):
    if not mel.connect_material_expressions(source, output, target, input_name):
        raise RuntimeError(f"connexion impossible : {source.get_name()} -> {target.get_name()}.{input_name}")


def clamp01(material, source, x, y):
    # Min(Max(x, 0), 1) plutôt que Saturate : les nœuds à une entrée se relient mal par script
    low = node(material, unreal.MaterialExpressionMax, x, y, const_b=0.0)
    connect(source, "", low, "A")
    high = node(material, unreal.MaterialExpressionMin, x + 100, y, const_b=1.0)
    connect(low, "", high, "A")
    return high


def create_overlay_material():
    """La texture du simulateur porte la couleur de l'heure d'arrivée (RVB) et cette heure codée dans l'alpha
    (255 = pas brûlé). Une cellule s'affiche si alpha <= Threshold ; le front des dernières minutes (FrontWidth,
    même codage) prend la couleur FrontColor. Les décalques ignorent la lumière émise : le front est dans la teinte."""
    path = f"{MATERIAL_DIR}/{MATERIAL_NAME}"
    if assets.does_asset_exist(path):
        material = assets.load_asset(path)
        # delete_all_material_expressions n'efface qu'une partie des nœuds : on recommence jusqu'à ce que ce soit vide,
        # sinon les nœuds orphelins de l'ancienne version font échouer la compilation.
        while expressions := mel.get_material_expressions(material):
            for expression in expressions:
                mel.delete_material_expression(material, expression)
    else:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            MATERIAL_NAME, MATERIAL_DIR, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    texture = node(material, unreal.MaterialExpressionTextureSampleParameter2D, -1000, 0,
                   parameter_name="FireTexture", texture=unreal.load_asset("/Engine/EngineResources/DefaultTexture"))
    threshold = node(material, unreal.MaterialExpressionScalarParameter, -1000, 300,
                     parameter_name="Threshold", default_value=1.0)
    front_width = node(material, unreal.MaterialExpressionScalarParameter, -1000, 450,
                       parameter_name="FrontWidth", default_value=0.05)
    front_color = node(material, unreal.MaterialExpressionVectorParameter, -400, 0,
                       parameter_name="FrontColor", default_value=unreal.LinearColor(1.0, 0.12, 0.0, 1.0))
    opacity = node(material, unreal.MaterialExpressionScalarParameter, -400, 200,
                   parameter_name="Opacity", default_value=0.85)

    # Âge du feu dans la cellule : Threshold - alpha, positif si elle a brûlé
    age = node(material, unreal.MaterialExpressionSubtract, -800, 300)
    connect(threshold, "", age, "A")
    connect(texture, "A", age, "B")
    # Brûlé : clamp(âge * 1000 + 1) vaut 1 si âge >= 0, 0 sinon (les alphas sont espacés de 1/255)
    scaled = node(material, unreal.MaterialExpressionMultiply, -650, 300, const_b=1000.0)
    connect(age, "", scaled, "A")
    shifted = node(material, unreal.MaterialExpressionAdd, -550, 300, const_b=1.0)
    connect(scaled, "", shifted, "A")
    burned = clamp01(material, shifted, -450, 300)
    # Front actif : 1 au passage du feu, 0 au bout de FrontWidth
    age_ratio = node(material, unreal.MaterialExpressionDivide, -650, 450)
    connect(age, "", age_ratio, "A")
    connect(front_width, "", age_ratio, "B")
    fading = node(material, unreal.MaterialExpressionSubtract, -550, 450, const_a=1.0)
    connect(age_ratio, "", fading, "B")
    front = clamp01(material, fading, -450, 450)

    color = node(material, unreal.MaterialExpressionLinearInterpolate, -200, 0)
    connect(texture, "RGB", color, "A")
    connect(front_color, "", color, "B")
    connect(front, "", color, "Alpha")
    visible = node(material, unreal.MaterialExpressionMultiply, -200, 250)
    connect(burned, "", visible, "A")
    connect(opacity, "", visible, "B")

    for source, prop in ((color, unreal.MaterialProperty.MP_BASE_COLOR), (visible, unreal.MaterialProperty.MP_OPACITY)):
        if not mel.connect_material_property(source, "", prop):
            raise RuntimeError(f"connexion impossible vers {prop}")
    mel.recompile_material(material)
    assets.save_asset(path)
    unreal.log(f"FireSim : matériau {path} créé")


def _fresh_material(name):
    path = f"{MATERIAL_DIR}/{name}"
    if assets.does_asset_exist(path):
        material = assets.load_asset(path)
        while expressions := mel.get_material_expressions(material):
            for expression in expressions:
                mel.delete_material_expression(material, expression)
    else:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, MATERIAL_DIR, unreal.Material, unreal.MaterialFactoryNew())
    return material, path


def create_aircraft_material():
    """Avions : les couleurs de la livrée sont celles des sommets du maillage."""
    material, path = _fresh_material("M_FireAircraft")
    colors = node(material, unreal.MaterialExpressionVertexColor, -400, 0)
    roughness = node(material, unreal.MaterialExpressionConstant, -400, 200, r=0.4)
    if not mel.connect_material_property(colors, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connexion impossible vers la couleur de base")
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connexion impossible vers la rugosité")
    mel.recompile_material(material)
    assets.save_asset(path)
    unreal.log(f"FireSim : matériau {path} créé")


def create_drop_material():
    """Gouttes des largages : eau ou retardant, translucides. La scène a l'exposition d'un plein soleil réel, où une couleur
    émise ordinaire paraîtrait noire : on compense l'exposition pour afficher la couleur telle quelle."""
    material, path = _fresh_material("M_FireDrop")
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    # Les gouttes sont un seul maillage répété : sans cet usage déclaré, le moteur les rend avec le matériau par défaut
    material.set_editor_property("used_with_instanced_static_meshes", True)
    color = node(material, unreal.MaterialExpressionVectorParameter, -400, 0,
                 parameter_name="Color", default_value=unreal.LinearColor(0.75, 0.88, 1.0, 1.0))
    opacity = node(material, unreal.MaterialExpressionScalarParameter, -400, 200,
                   parameter_name="Opacity", default_value=0.6)
    exposure = node(material, unreal.MaterialExpressionEyeAdaptationInverse, -400, 120)
    shown = node(material, unreal.MaterialExpressionMultiply, -200, 50)
    connect(color, "", shown, "A")
    connect(exposure, "", shown, "B")
    if not mel.connect_material_property(shown, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("connexion impossible vers la couleur")
    if not mel.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY):
        raise RuntimeError("connexion impossible vers l'opacité")
    mel.recompile_material(material)
    assets.save_asset(path)
    unreal.log(f"FireSim : matériau {path} créé")


def create_map():
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    map_file = unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.project_content_dir() + MAP_PATH.removeprefix("/Game/") + ".umap")
    if os.path.exists(map_file):
        if not level_editor.load_level(MAP_PATH):
            raise RuntimeError(f"impossible d'ouvrir la carte {MAP_PATH}")
    elif not level_editor.new_level(MAP_PATH):
        raise RuntimeError(f"impossible de créer la carte {MAP_PATH}")

    simulator = next((a for a in actors.get_all_level_actors() if isinstance(a, unreal.FireSimulator)), None)
    if simulator is None:
        simulator = actors.spawn_actor_from_class(unreal.FireSimulator, unreal.Vector(0, 0, 0))
        simulator.set_actor_label("Simulateur de feu")
    simulator.prepare_scene()
    if not level_editor.save_current_level():
        raise RuntimeError(f"impossible d'enregistrer la carte {MAP_PATH}")
    unreal.log(f"FireSim : carte {MAP_PATH} : {simulator.get_editor_property('status')}")

# Simulateur de propagation de feux de forêt — Corse

Simulation de feux sur la Corse en 3D : départ de feu, propagation selon la végétation,
le relief, le vent et la météo (modèle de Rothermel), lutte contre le feu, carte de risque.

| Étape | Contenu | État |
|---|---|---|
| 0 | Données : relief, végétation, eau, coupures | **fait** |
| 1 | Module de simulation C++ (Rothermel + propagation) | **fait** |
| 2 | Application Unreal 5 + Cesium | **fait** : terrain IGN sur ion, feu drapé sur le relief, panneau de réglage en jeu |
| 3 | Météo réelle, vent corrigé par le relief, rejeu de feux historiques | **fait** : 7 feux rejoués (2017 à 2026) ; carte de la végétation corrigée par sa hauteur mesurée, vitesse calibrée (voir « Calibration ») |
| 4 | Lutte : pare-feu, largages | à faire |
| 5 | Carte de risque (milliers de départs sur le serveur lab) | à faire |
| 6 | Visuels : flammes, fumée | à faire |

## Décisions de cadrage

- **Physique** : Rothermel (vitesse de propagation) sur une grille de 25 m ; sautes de feu et feux de cimes plus tard.
- **Architecture** : le calcul du feu est un module C++ autonome, sans dépendance à Unreal, compilé
  en plugin Unreal et en outil en ligne de commande (carte de risque sur le serveur lab).
- **Moteur** : Unreal Engine 5.8 + Cesium for Unreal ; photo aérienne IGN BD ORTHO en flux WMTS.
- **Météo** : réglages manuels, ou météo réelle via Open-Meteo : prévisions et archives des prévisions de Météo-France
  (AROME, depuis novembre 2022), réanalyse ERA5 avant, pour rejouer des feux réels.
  Vent corrigé par le relief avec WindNinja.
- **Première version** : timeline et contours horaires, lutte simple (pare-feu, largages sans logistique),
  carte de risque. Rendu : couleurs sur le terrain d'abord, flammes et fumée (Niagara) ensuite.

## Étape 0 — pipeline de données

Prérequis : [uv](https://docs.astral.sh/uv/). Rien d'autre à installer (GDAL est fourni par rasterio).

```bash
uv sync --project pipeline
uv run --project pipeline python -I -m firesim_pipeline.cli all
```

Les étapes peuvent aussi tourner une par une : `download`, `dem`, `barriers`, `canopy`, `fuel`, `export`, `quicklook`.
3,4 Go téléchargés, 8,3 Go dans `data/raw/` une fois décompressés, 1,6 Go produits.
Hors téléchargement, le traitement complet prend environ 9 minutes. Pour mettre les données
sur un autre disque : variable d'environnement `FIRESIM_DATA_DIR`.

Contrôles faits sur le résultat : 8 769 km² de terre (Corse officielle : 8 680 km², l'écart vient
des cellules côtières et des îlots) ; point culminant à 2 701 m aux coordonnées du Monte Cinto
(2 706 m, la moyenne sur 25 m écrête le sommet).

### Ce qui est produit (`data/processed/`)

Toutes les couches partagent la même grille, décrite dans `grid.json` :
Lambert 93 (EPSG:2154), cellules de 25 m, origine en haut à gauche.

| Fichier | Type | Contenu |
|---|---|---|
| `elevation.tif` | float32 | Altitude (m, IGN78C), NaN en mer |
| `slope.tif` | float32 | Pente (degrés) |
| `aspect.tif` | float32 | Orientation : direction vers laquelle le terrain descend, en degrés depuis le nord (sens horaire) ; -1 = plat |
| `fuel_model.tif` | uint8 | Modèle de combustible Scott & Burgan (code numérique, voir `pipeline/config/fuel_models.yaml`) ; 0 = mer |
| `vegetation_height.tif` | uint8 | Hauteur dominante de la végétation (m) : 90e centile des hauteurs à 5 m dans la cellule |
| `tree_cover.tif` | uint8 | Part de la cellule couverte d'arbres d'au moins 5 m (%) |
| `water.tif` | uint8 | 1 = eau libre |
| `barrier_width.tif` | uint8 | Largeur (m) de la coupure la plus large de la cellule (rivière, route, chemin) ; 0 = aucune |
| `terrain/dem_5m.tif` | float32 | Relief 5 m pour le terrain 3D (à envoyer sur Cesium ion, référence d'altitude « niveau moyen des mers ») |
| `sim/` | binaire brut | Les mêmes couches pour le module C++ (`meta.json` + un fichier par couche, petit-boutiste, ligne par ligne depuis le nord-ouest) |
| `quicklook/*.png` | | Aperçus de contrôle |

### Hypothèses à calibrer

Elles sont toutes dans `pipeline/config/` :

- `fuel_models.yaml` : traduction de la végétation (BD Forêt, puis WorldCover) en modèles de combustible.
  Une forêt fermée dont le couvert d'arbres mesuré reste sous 40 % est traitée comme une forêt ouverte, en maquis
  (voir « Calibration »). Résultat sur l'île : 58 % maquis (SH5), 25 % forêt à sous-bois dense (TU5), 6 % herbe,
  4 % pinèdes d'altitude.
  Points les plus incertains :
  - le maquis : SH5 partout, alors qu'il faudrait SH7 pour le maquis haut. La carte de hauteur ne le permet pas :
    elle donne 0 m à la moitié des cellules de maquis, car elle repère les arbres, pas les arbustes ;
  - les landes d'altitude (au-dessus de ~1 400 m) sont aussi en SH5, alors qu'elles sont nettement moins chargées.
    Il faudra une règle selon l'altitude.
- `barriers.yaml` : largeur des rivières et routes. Un ruisseau intermittent compte pour moitié (à sec en été).
  Environ 20 % des cellules contiennent au moins un sentier ou un ruisseau de 1 à 3 m : c'est à la simulation
  de les traiter comme des coupures faibles, franchies dès que les flammes dépassent quelques mètres.

## Étape 1 — module de simulation (`sim/`)

Bibliothèque C++20 sans aucune dépendance (`firesim_core`), un outil en ligne de commande (`firesim`)
et les tests. Sous Windows, `sim\build.bat` compile et lance les tests avec le compilateur de Visual Studio ;
sous Linux : `cmake -S sim -B sim/build -G Ninja && cmake --build sim/build && ctest --test-dir sim/build`.

```bash
# Un feu dans le Nebbio, libeccio à 45 km/h, sécheresse extrême, 6 heures
sim/build/firesim run --ignition 42.62,9.355 --wind 45 --wind-from 225 --moisture extreme --duration 6h --out runs/nebbio
# Vent qui tourne en cours de route : météo heure par heure
sim/build/firesim run --ignition 42.62,9.355 --weather sim/examples/meteo_bascule_vent.csv --duration 8h --out runs/bascule
# Météo réelle d'un jour donné (voir l'étape 3)
sim/build/firesim run --ignition 42.62,9.355 --meteo runs/meteo.csv --start 2025-07-20T14:00 --duration 8h --out runs/reel
# Aperçu PNG et GeoTIFF du résultat
uv run --project pipeline python -I -m firesim_pipeline.runview runs/nebbio
# Comportement du feu sur terrain homogène
sim/build/firesim point --fuel SH5 --moisture extreme --wind 40 --slope 15
```

Un feu de 1 000 à 2 000 ha se calcule en 0,1 à 0,2 s (plus 0,3 s de chargement de la carte).
Le dossier de sortie contient `run.json` (paramètres, résumé, emprise) et, sur l'emprise du feu, l'heure
d'arrivée (min), la vitesse (m/min), la longueur de flamme (m) et l'intensité (kW/m) en binaire brut.

**Modèle.**
- Vitesse en chaque point : Rothermel (1972) avec les compléments d'Albini (1976), exactement comme behave,
  le moteur de BehavePlus.
- Forme du feu : ellipse de propagation.
- Propagation : plus court chemin en temps sur 16 voisins (erreur de forme < 3 %), avec la météo de l'instant
  où le feu atteint chaque cellule.
- Coupures : une coupure de largeur L arrête le feu si L ≥ 1,5 × la longueur de flamme dans cette direction.
  Un sentier arrête donc un feu de flanc, mais pas un front de tête dans le maquis.
- Vitesse calibrée : la vitesse de Rothermel est multipliée par 0,44, d'après les feux réels (voir « Calibration »).
  `--ros-factor 1` revient à Rothermel seul. `firesim point` donne toujours Rothermel seul.
- Autres réglages pour la calibration, sans effet par défaut : `--wind-factor` (vent), `--canopy-cover` (couvert des
  arbres), `--fuel-swap TU5=SH5` (brûler un modèle comme un autre).

**Validation.**
- Avant le facteur de vitesse, les vitesses, intensités, flammes et formes d'ellipse reproduisent behave à 10⁻⁶ près, sur 4 920 cas :
  40 modèles × 5 humidités × vents × pentes × couvert. Les valeurs de référence sont générées par
  `sim/tools/behave_reference` à partir du code de behave (domaine public).
- La propagation est testée sur cas simples : cercle sans vent, ellipse de Rothermel avec vent,
  montée de pente, coupure qui tient ou cède, ligne d'eau en diagonale infranchissable.

**Limites actuelles.**
- Pas de sautes de feu (brandons) ni de feux de cimes : les feux par vent fort sont sous-estimés.
- Couvert forestier supposé (50 %, 15 m) au-dessus des modèles de sous-bois : le couvert mesuré sert à classer les
  forêts, pas encore à régler l'abri du vent cellule par cellule.

## Étape 2 — terrain 3D sur Cesium ion

Le relief 5 m (`data/processed/terrain/dem_5m.tif`, 1 Go) devient un terrain 3D Cesium, posé sur le terrain
mondial de Cesium (qui prend le relais en mer et hors de l'île). L'envoi passe par l'API d'ion avec un jeton,
plus fiable que le site pour un fichier de cette taille :

```powershell
# Jeton créé sur https://ion.cesium.com/tokens avec les portées assets:read et assets:write
$env:CESIUM_ION_TOKEN = "<jeton>"
uv run --project pipeline python -I -m firesim_pipeline.cli ion
```

Le script crée l'asset (type Raster Terrain, altitudes par rapport au niveau de la mer), envoie le fichier,
puis suit la fabrication des tuiles et affiche l'identifiant de l'asset.
L'identifiant est gardé dans `data/processed/terrain/ion_asset.json` : relancer la commande reprend le suivi
sans renvoyer le fichier, et le projet Unreal le lit pour charger le terrain.

### Projet Unreal (`unreal/`)

- `FireSimCorse.uproject` : Unreal Engine 5.8, Cesium for Unreal 2.31. Le champ `EngineAssociation` contient
  l'identifiant que l'éditeur s'est donné sur ce PC (le lanceur Epic n'a pas enregistré l'installation de la 5.8) :
  sur un autre PC, le remettre à `5.8` ou choisir la version par clic droit sur le fichier → « Switch Unreal Engine version ».
  Le fichier doit rester sans accents : Unreal le réenregistre sinon en UTF-16, illisible pour son outil de compilation.
- Plugin `Plugins/FireSim` : l'acteur **Simulateur de feu** (`AFireSimulator`). Le module de simulation `sim/`
  y est compilé tel quel par la chaîne d'Unreal ; il lit les mêmes couches que l'outil en ligne de commande
  (`data/processed/sim`). Le feu est affiché par un décalque sur le terrain : couleur selon l'heure d'arrivée,
  front actif lumineux. En jeu, le panneau de réglage est dans `FireSimPanel.cpp`.
- `Scripts/create_assets.py` : crée le matériau du calque de feu et la carte `Corse` (géoréférence centrée
  sur la Corse, terrain IGN depuis Cesium ion, imagerie, soleil, caméra de Cesium). La construction elle-même
  est dans `Scripts/firesim_assets.py`, réutilisable depuis l'éditeur ouvert.
- Imagerie : Bing Maps Aerial (Cesium ion) dessous, photo aérienne IGN (WMTS de la Géoplateforme) dessus.
  Au lancement du jeu, une tuile de test vérifie que la Géoplateforme répond : sinon, le calque IGN reste coupé
  (imagerie Bing seule, message dans le journal), et il faut relancer le jeu pour réessayer. Le calque charge
  au plus 4 tuiles à la fois : le 01/11/2026, avec 20 (le défaut de Cesium), le serveur a cessé de répondre à notre
  adresse IP après un millier de tuiles en 3 minutes, et ne répondait toujours pas 2 h 30 plus tard.
  Limite connue : au-dessus de la mer, à partir du niveau de zoom 13, l'IGN renvoie des tuiles blanches opaques
  (JPEG, sans transparence) qui masquent Bing ; il faudrait un matériau Cesium qui rende le blanc pur transparent.
- Calque du feu : la couleur suit l'heure d'arrivée (jaune au départ, violet pour les dernières heures) ; le front
  des 20 dernières minutes est orange vif. Les décalques ignorent la lumière émise, d'où une couleur plutôt qu'une lueur.

Prérequis de compilation (Visual Studio Build Tools 2026) : outils C++, kit de développement .NET Framework 4.8.1,
pack de ciblage .NET Framework 4.6.2, et de préférence MSVC 14.50 (le compilateur conseillé pour UE 5.8).

```powershell
# Compilation
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' FireSimCorseEditor Win64 Development '-Project=<dépôt>\unreal\FireSimCorse.uproject' -WaitMutex
# Matériau et carte (une fois)
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' '<dépôt>\unreal\FireSimCorse.uproject' -run=pythonscript '-script=<dépôt>\unreal\Scripts\create_assets.py'
```

Le script peut être relancé : il reconstruit le matériau et complète la carte sans rien dupliquer.
`Config/DefaultEngine.ini` reprend les réglages de rendu du modèle « Simulation » d'Unreal (Lumen, plage de
luminance étendue) : sans eux, le soleil de Cesium sature l'image en blanc.
`Scripts/pilotage.py`, lancé à l'ouverture de l'éditeur par `Content/Python/init_unreal.py`, exécute les scripts
Python déposés dans `Saved/Pilotage` : c'est ainsi que Claude Code pilote l'éditeur ouvert (caméra, simulation,
captures d'écran). Supprimer `init_unreal.py` pour le désactiver.
Contrôle fait dans le jeu : le rejeu d'Oletta donne les mêmes 235 ha dans Unreal et dans l'outil en ligne de
commande, et le décalque se place bien sur l'emprise du feu.

Dans l'éditeur, une seule fois : panneau Cesium → **Connect to Cesium ion**, pour que le terrain se charge.
Cesium garde alors les jetons d'accès en clair dans `Content/CesiumSettings/` : ce dossier n'est pas versionné, et
Cesium le recrée sur une copie neuve du projet.
Ensuite :
- **Dans l'éditeur** : sélectionner « Simulateur de feu », régler départ, vent, humidité, durée, puis **Simulate**.
  Chaque réglage relance le calcul ; « Displayed Hours » fait défiler le feu.
- **En jeu** (Play) : le feu part du point réglé et progresse (0,25 h par seconde). Un panneau en haut à gauche
  (`SFireSimPanel`, en Slate) règle le vent (vitesse, direction, correction par le relief), la sécheresse de la
  végétation et la durée : le feu est recalculé au relâchement du curseur (0,1 à 0,2 s). La partie « Lecture »
  fait défiler le feu heure par heure (lecture, pause, vitesse) et donne la surface brûlée à l'heure affichée.
  La souris reste visible : glisser dans la vue pour regarder autour, flèches pour avancer et tourner, E / C pour
  monter et descendre, **F** pour allumer un feu sous la souris. **H** masque le panneau et repasse en vol libre
  (la souris tourne la caméra, F allume au centre de la vue).
- **Météo réelle** (panneau « Météo : Réelle », ou case « Real Weather » dans l'éditeur) : date (flèches, saisie
  JJ/MM/AAAA puis Entrée, ou « Maintenant ») et heure du départ. La météo est téléchargée en une seconde environ
  (`Private/RealWeather.cpp`), puis réutilisée tant que le départ reste à moins de 3 km. Pendant la lecture, le vent,
  la température, l'humidité de l'air et celle des brindilles affichés suivent l'heure du feu.
- **Rejouer un feu réel** (haut du panneau) : un bouton par feu de `data/processed/feux` (étape 3). Le feu repart du
  même point, à la même heure, avec la météo réelle ; la zone réellement brûlée est dessinée en bleu sur le terrain,
  et le panneau donne à l'heure affichée la surface simulée, le recouvrement avec le feu réel et les surfaces
  annoncées pendant le feu.

## Étape 3 — vent corrigé par le relief, météo réelle

Le vent saisi (ou issu de la météo) est le vent « général ». Sur le terrain, le relief l'accélère sur les crêtes,
le freine sous le vent et le canalise dans les vallées. Ce vent local vient d'un atlas calculé une fois pour toutes
avec [WindNinja](https://github.com/firelab/windninja) (USFS), le modèle de vent utilisé avec FARSITE et FlamMap.

```bash
# Prérequis : WindNinja installé (C:\WindNinja ou Program Files), ou FIRESIM_WINDNINJA=<chemin de WindNinja_cli.exe>
uv run --project pipeline python -I -m firesim_pipeline.cli wind
```

- 16 directions de vent général, solveur à conservation de la masse, maillage de 200 m, rugosité « broussailles ».
- 48 s par direction sur 16 cœurs, 13 minutes en tout ; reprise possible si on l'interrompt.
  Aperçu : `data/processed/quicklook/wind_atlas.png`.
- Résultat : `data/processed/sim/wind/` (composantes est/nord du vent local pour un vent général unité, grille de 200 m).
  Ce solveur est linéaire : le rapport vent local / vent général ne dépend pas de la vitesse, un calcul par direction suffit.
- Le module C++ charge l'atlas automatiquement s'il existe. Entre deux directions, il interpole la vitesse et la déviation.
  `--no-terrain-wind` revient au vent uniforme.
- Directions : le vent est donné par rapport au nord géographique, alors que les pentes sont calculées sur la grille
  Lambert 93, tournée d'environ 4,5° en Corse (convergence des méridiens). WindNinja en tient compte, et le vent
  uniforme est corrigé de cet angle.

**Résultat.** Le vent local va de 0,4 à 1,8 fois le vent général (1er et 99e centiles), jusqu'à 3,4 fois sur
les crêtes les plus exposées ; il reste à 0,98 sur la plaine orientale, qui est plate. Par vent du nord-ouest,
les crêtes orientées nord-est/sud-ouest, en travers du vent, accélèrent nettement et les versants abrités freinent ;
par libeccio (sud-ouest), parallèle à ces crêtes, ce sont surtout les vallées qui canalisent le vent.

Sur le feu du Nebbio (libeccio 45 km/h, sécheresse extrême, 6 h) :

| | Vent uniforme | Vent corrigé par le relief |
|---|---|---|
| Surface brûlée | 1 432 ha | 1 266 ha |
| Vitesse maximale | 3,7 km/h | 6,1 km/h |
| Flammes | 8 m | 10 m |

Avec le relief, le feu démarre lentement dans un creux abrité (22 ha la première heure, vent local à 0,7 fois le vent
général), puis file vers l'est-nord-est le long d'une crête où le vent atteint 1,5 fois le vent général.
Moins de surface, mais un front de tête plus rapide et plus intense.

### Météo réelle

À la place d'un vent et d'une sécheresse réglés à la main : la météo d'un jour et d'une heure donnés, au point
de départ du feu.

```bash
uv run --project pipeline python -I -m firesim_pipeline.meteo --at 42.62,9.355 --start 2025-07-20T14:00 --duration 8h --out runs/meteo.csv
sim/build/firesim run --ignition 42.62,9.355 --meteo runs/meteo.csv --start 2025-07-20T14:00 --duration 8h --out runs/reel
```

- **Source** : [Open-Meteo](https://open-meteo.com), en heure locale.
  - Du 13 novembre 2022 aux prévisions des 4 prochains jours : archives des prévisions de Météo-France, grandeur
    par grandeur AROME 1,3 km, sinon AROME 2,5 km, sinon ARPEGE 10 km.
  - Avant, ou s'il manque des heures : la réanalyse ERA5 (30 km, depuis 1940, jusqu'à 5 jours avant aujourd'hui),
    dont le vent est très lissé par le relief.
- **Grandeurs** : vent moyen et direction à 10 m, température et humidité de l'air à 2 m, rayonnement solaire, pluie.
  Le vent change toutes les heures, et celui de chaque heure passe par la correction du relief (WindNinja).
- **Humidité des végétaux morts**, heure par heure (`dead_fuel_moisture` dans `sim/src/weather.cpp`) :
  - équilibre avec l'air d'après Simard (1968), comme le NFDRS américain ;
  - au ras du sol, le soleil réchauffe et assèche les végétaux : correction du NFDRS de 1978 (+25 °F et humidité
    de l'air x 0,75 en plein soleil), interpolée selon le rayonnement ;
  - brindilles (1 h), branches (10 h) et grosses branches (100 h) rejoignent cet équilibre en 1, 10 et 100 heures ;
    sous la pluie, elles tendent vers la saturation (35 %) ;
  - le calcul démarre 14 jours avant le feu, pour que les grosses branches aient eu le temps de s'ajuster.
- **Végétation vivante** : toujours réglée par le scénario de sécheresse (`--moisture`), faute de mesure.

Au départ du Nebbio à 14 h, sur 8 h :

| Jour | Source | Air | Brindilles mortes | Surface |
|---|---|---|---|---|
| 24/07/2017 | ERA5 | 30 °C, 29 %, vent d'ouest 26 km/h | 4 % | 1 756 ha |
| 20/07/2025 | AROME | 27 °C, 73 %, vent de nord-ouest 15 km/h | 9 % | 94 ha |

Limites :
- la météo est prise au seul point de départ, sans champ de vent variable sur la zone ;
- AROME à 1,3 km voit déjà une partie du relief, que WindNinja corrige une seconde fois ;
- la végétation vivante ne suit pas la saison.

### Rejeu de feux réels

Le simulateur repart du point et de l'heure de départ d'un feu réel, avec la météo du jour, et son résultat est
comparé à la zone brûlée cartographiée par EFFIS (Copernicus) sur images satellite.

```bash
uv run --project pipeline python -I -m firesim_pipeline.rejeu                 # feux du catalogue
uv run --project pipeline python -I -m firesim_pipeline.rejeu cagnano-2026    # un feu, ou « all »
```

- Catalogue : `pipeline/config/feux_historiques.yaml`. Pour chaque feu : heure et point de départ, durée, végétation
  vivante, identifiant du contour EFFIS, surfaces annoncées pendant le feu, sources.
  - Le point de départ est celui que donne la presse (lieu-dit, route) quand elle le donne. Sinon, c'est la pointe
    du contour d'où venait le vent au départ.
  - L'heure est arrondie à l'heure, car la météo est horaire.
  - Pour ajouter un feu, chercher son identifiant dans la couche EFFIS de son année (`modis.ba.poly.<année>`, ou
    `modis.ba.poly.season` pour l'année en cours) puis relancer la commande.
- Résultat dans `runs/rejeu/<feu>/` : météo, simulation, `comparaison.json` et `comparaison.png` (contour réel en bleu
  sur le feu simulé). Le feu et son contour sont aussi écrits dans `data/processed/feux/` pour Unreal.
- Mesure de l'accord : l'indice de Sørensen (2 × surface commune / somme des deux surfaces), 1 si les deux zones
  coïncident. Il est donné à la fin de la simulation et au moment où le feu simulé atteint la surface réelle : ce
  second chiffre juge la forme et la direction, indépendamment de la vitesse.

| Feu | Météo | Réel (EFFIS) | Simulé | Sørensen à la fin | à surface égale |
|---|---|---|---|---|---|
| Olmeta-di-Tuda, 24/07/2017, 24 h | ERA5 | 2 430 ha | 3 521 ha | 0,75 | 0,84 (au bout de 12,9 h) |
| Ogliastro et Sisco, 11/08/2017, 24 h | ERA5 | 1 491 ha | 10 494 ha | 0,25 | 0,23 (7,6 h) |
| Santo-Pietro-di-Tenda, 01/08/2022, 24 h | ERA5 | 490 ha | 1 846 ha | 0,41 | 0,48 (8,6 h) |
| Saint-Florent, 05/10/2025, 12 h | AROME | 216 ha | 345 ha | 0,76 | 0,65 (5,6 h) |
| Oletta, 05/10/2025, 12 h | AROME | 202 ha | 235 ha | 0,74 | 0,74 (8,6 h) |
| Biguglia, 25/07/2026, 24 h | AROME | 184 ha | 2 252 ha | 0,15 | 0,54 (2,5 h) |
| Cagnano, 29/08/2026, 9 h | AROME | 121 ha | 463 ha | 0,34 | 0,20 (2,8 h) |

Surfaces annoncées pendant les feux, comparées au simulé à la même heure :
- **Olmeta** : 1 500 ha le lendemain à 9 h (simulé 3 371).
- **Ogliastro** : 100 ha à 7 h 30 (simulé 1 078), 1 500 ha à 22 h 30 (simulé 9 518).
- **Santo-Pietro** : 450 ha le lendemain à 8 h (simulé 1 726).
- **Oletta** : 160 ha à 18 h (simulé 147).
- **Saint-Florent** : 220 ha à 18 h (simulé 300).
- **Biguglia** : 3,5 ha à 16 h 36 (simulé 96).
- **Cagnano** : 12, 45, 100, 130 ha à 16 h 30, 17 h 30, 19 h, 23 h (simulé 88, 177, 298, 463).

Lecture d'ensemble :
- **Oletta et Saint-Florent collent au réel**, en surface comme en forme : ce sont les deux feux où le vent d'AROME est net et où le simulateur va à peu près à la vitesse annoncée.
- **Le simulateur ne connaît pas encore la lutte** : il va plus vite que les surfaces annoncées partout sauf à Oletta. L'écart est le plus fort là où l'attaque a été massive et immédiate : Biguglia (3,5 ha au bout de 1 h 30 près des maisons, simulé 96) et Santo-Pietro (plus de 160 largages dès le premier jour). Le rejeu mesure donc aussi ce que la lutte a sauvé, ce qui servira à l'étape 4.
- **Avec ERA5, le vent est trop lisse** (30 km) pour étirer le feu : l'axe est bon à Santo-Pietro et Ogliastro, mais le feu simulé est beaucoup trop large. Ogliastro est le cas le plus net : un feu réel en longue langue de 10 km, un feu simulé sept fois plus grand autour du même axe.
- **Cagnano** : le feu simulé descend maintenant dans la langue sud du feu réel, classée autrefois en forêt lente. Il reste trop large vers le nord et l'est, où les pompiers tenaient le flanc côté marine, et il franchit la RD 132, que le feu réel n'a pas passée.

### Calibration

```bash
uv run --project pipeline python -I -m firesim_pipeline.calibration    # après firesim_pipeline.rejeu all
```

Résultats dans `runs/calibration/` : `essais.csv` (chaque simulation) et `synthese.json`. L'outil essaie 60 réglages
sur 5 feux (forêts à sous-bois avec ou sans couvert, ou brûlant comme du maquis ; vent ×0,75 à ×1,5 ; vitesse ×0,2 à
×1), en quelques minutes.

Deux critères, parce que la lutte réduit la surface finale des feux réels :
- la **forme** : recouvrement au moment où le feu simulé atteint la surface réelle ;
- la **vitesse** : surfaces simulées / surfaces annoncées pendant le feu.

**1. Sur l'ancienne carte, aucun réglage commun ne tenait.** La forme moyenne tombait de 0,49 à 0,39 quand chaque feu
était jugé avec des réglages calibrés sans lui. Traiter toute la « forêt » comme du maquis aidait Santo-Pietro et
Olmeta, mais dégradait Ogliastro et Saint-Florent : cette classe mélange de vraies futaies et du maquis arboré.

**2. La hauteur mesurée sépare les deux.** L'étape `canopy` lit la carte de hauteur du couvert de Meta et du WRI (1 m,
images de 2018-2019). Règle : une forêt fermée (TU5) à moins de 40 % d'arbres d'au moins 5 m est traitée comme une
forêt ouverte, en maquis (SH5). Le seuil de 40 % est celui de la BD Forêt pour une forêt fermée ; il n'a pas été ajusté
sur les feux. 52 % des forêts TU5 changent de classe. Les feux réels donnent raison à cette séparation, sans passer
par le simulateur :

| Part de chaque végétation brûlée autour du feu | Forêt claire | Forêt fermée | Maquis |
|---|---|---|---|
| Santo-Pietro 2022 | 26 % | 23 % | 18 % |
| Saint-Florent 2025 | 14 % | 2 % | 23 % |
| Oletta 2025 | 19 % | 6 % | 14 % |
| Biguglia 2026 | 8 % | 1 % | 14 % |
| Cagnano 2026 | 12 % | 0 % | 6 % |

**3. La vitesse devient calibrable.** Avec la carte corrigée, le feu va 14 fois trop vite (médiane des feux) : deux
erreurs se compensaient, un maquis SH5 trop rapide et une moitié de ce maquis classée en forêt lente. La lutte ne peut
que ralentir un feu, donc le simulateur doit aller au moins aussi vite que le moins combattu. On retient le plus petit
facteur de vitesse qui ne sous-estime aucun feu : **×0,44**, fixé par Oletta, parti alors que les moyens étaient
sans doute déjà engagés à Saint-Florent. Le vent et le couvert restent à leurs valeurs d'origine : aucun autre réglage
ne fait mieux.

Les feux de 2017 ne comptent pas dans ce choix : les images datent de 2019, la zone brûlée y apparaît déjà rase, et la
calibration y trouverait d'avance la réponse.

| Feu | Forme avant → après | Sørensen à la fin avant → après | Vitesse avant → après | Forme et fin, jugé sans lui |
|---|---|---|---|---|
| Santo-Pietro 2022 | 0,23 → 0,48 | 0,17 → 0,41 | ×10 → ×3,8 | 0,48 ; 0,41 |
| Saint-Florent 2025 | 0,78 → 0,65 | 0,53 → 0,76 | ×2,0 → ×1,4 | 0,68 ; 0,75 |
| Oletta 2025 | 0,64 → 0,74 | 0,65 → 0,74 | ×0,8 → ×0,9 | 0,30 ; 0,30 |
| Biguglia 2026 | 0,60 → 0,54 | 0,11 → 0,15 | ×53 → ×28 | 0,54 ; 0,15 |
| Cagnano 2026 | 0,16 → 0,20 | 0,20 → 0,34 | ×1,2 → ×4,2 | 0,20 ; 0,34 |
| **Moyenne** | **0,48 → 0,52** | **0,33 → 0,48** | | **0,44 ; 0,39** |
| *Olmeta 2017, non jugé* | 0,60 → 0,84 | 0,54 → 0,75 | ×3,2 → ×2,3 | |
| *Ogliastro 2017, non jugé* | 0,43 → 0,22 | 0,40 → 0,25 | ×1,9 → ×8,3 | |

Ce qu'il faut garder en tête :
- **Le gain est net sur le recouvrement final** : les cinq feux progressent. Il est faible sur la forme : trois feux
  progressent, Saint-Florent et Biguglia reculent.
- **Le facteur ×0,44 repose sur un seul feu, Oletta.** Calibré sans lui, le facteur tombe à ×0,36 avec un vent ×0,75,
  et Oletta n'atteint plus sa surface (forme 0,30) : c'est ce qui fait baisser la moyenne en validation croisée. Il
  faudra d'autres feux peu combattus pour le confirmer.
- **Cagnano va maintenant plus vite qu'annoncé** (×4,2, contre ×1,2 avant) : l'accord d'avant venait de la compensation
  entre les deux erreurs.
- **Ogliastro se dégrade**, mais sa végétation est mesurée après le feu et son vent vient d'ERA5 : il ne permet pas de
  conclure dans un sens ou dans l'autre.
- **Le facteur s'applique à tous les combustibles**, alors que seul le maquis est vraiment mesuré ici : l'herbe et les
  pinèdes d'altitude ne brûlent presque pas dans ces sept feux. Il réduit aussi l'intensité et les flammes, donc les
  coupures tiennent mieux. Quand l'étape 4 ajoutera la lutte, il faudra le recalibrer.

## Sources et licences

| Donnée | Source | Licence |
|---|---|---|
| Relief 5 m | IGN RGE ALTI 2.0 (2020) | Licence Ouverte Etalab 2.0 |
| Végétation | IGN BD Forêt v2 (2016-2017) | Licence Ouverte Etalab 2.0 |
| Eau, routes | IGN BD TOPO 3.5 (septembre 2026) | Licence Ouverte Etalab 2.0 |
| Occupation du sol | ESA WorldCover 10 m 2021 | CC BY 4.0 — © ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium |
| Hauteur de la végétation | Meta et World Resources Institute, High Resolution Canopy Height Maps (images 2018-2019) | CC BY 4.0 |
| Météo | Open-Meteo : prévisions Météo-France AROME et ARPEGE, réanalyse ERA5 (Copernicus) | CC BY 4.0 — Weather data by Open-Meteo.com ; Météo-France : Licence Ouverte Etalab 2.0 ; ERA5 : licence Copernicus |
| Zones brûlées | EFFIS, European Forest Fire Information System (Copernicus Emergency Management Service) | Données Copernicus, libres d'usage avec mention de la source |

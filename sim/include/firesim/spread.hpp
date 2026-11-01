#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "firesim/landscape.hpp"
#include "firesim/rothermel.hpp"
#include "firesim/weather.hpp"

// Propagation sur la grille : on calcule l'heure d'arrivée du feu dans chaque cellule
// (plus court chemin en temps, à la Dijkstra, sur 16 voisins). Les conditions météo sont celles
// de l'instant où le feu atteint la cellule, ce qui permet au vent de tourner en cours de route.
namespace firesim {

// Effet de la lutte sur une cellule. Un feu qui s'y présente entre from_min et until_min, avec des flammes d'au plus
// max_flame_m, est retenu pendant hold_min, et jamais au-delà de until_min ; ensuite il repart.
// Voir firesim/suppression.hpp.
struct Treatment {
    uint32_t cell = 0;
    float from_min = 0, until_min = 0;
    float max_flame_m = 0;
    int32_t line = -1;  // chantier de pare-feu : si le feu atteint une cellule pas encore faite, la suite est perdue
    float hold_min = std::numeric_limits<float>::infinity();
};

struct SpreadOptions {
    double duration_min = 360;
    // Une coupure (route, rivière) de largeur L résiste si L >= breach_factor x longueur de flamme.
    double breach_factor = 1.5;
    // Couvert supposé au-dessus des modèles de sous-bois (TU, TL) ; le couvert mesuré ne sert qu'à classer les forêts.
    std::optional<Canopy> canopy = Canopy{0.5, 15.0 / kMetersPerFoot, 0.4};
    bool wind_limit = false;
    // Vent corrigé par le relief (atlas WindNinja) quand la carte en a un ; sinon vent uniforme.
    bool terrain_wind = true;
    // Ajustements, à la manière des facteurs de FARSITE, pour coller aux feux réels.
    // Vitesse : le plus petit facteur qui ne sous-estime aucun feu du catalogue (firesim_pipeline.calibration).
    double ros_factor = 0.44;  // multiplie la vitesse du feu (et donc son intensité)
    double wind_factor = 1.0;  // multiplie le vent
    // Modèles remplacés par d'autres (ex. TU5 -> SH5) : essayer une autre traduction de la végétation
    // sans réexporter la carte.
    std::vector<std::pair<uint8_t, uint8_t>> fuel_swap;
    std::vector<Treatment> treatments;
};

struct Ignition {
    double x, y;  // Lambert 93
    double time_min = 0;
};

struct BurnedCell {
    uint32_t index;
    float arrival_min;
    float ros_m_min;       // vitesse maximale du feu dans la cellule
    float flame_m;         // longueur de flamme du front de tête
    float intensity_kw_m;  // intensité du front de tête
    float direction_deg;   // direction de propagation maximale
};

// Ce que le feu a fait à un chantier de pare-feu ; infini si cela n'est pas arrivé.
struct LineOutcome {
    float cut_min = std::numeric_limits<float>::infinity();     // le feu a atteint une partie pas encore construite
    float cut_from = std::numeric_limits<float>::infinity();    // les cellules prévues à partir de cette heure ne seront pas faites
    float breach_min = std::numeric_limits<float>::infinity();  // le feu a franchi une partie terminée : flammes trop hautes
};

struct SpreadResult {
    std::vector<BurnedCell> cells;  // dans l'ordre d'arrivée du feu
    double area_ha = 0;
    bool reached_grid_edge = false;
    int col_min = 0, row_min = 0, col_max = -1, row_max = -1;
    std::vector<LineOutcome> lines;  // un par chantier de pare-feu
};

class Simulator {
public:
    explicit Simulator(const Landscape& land);

    // Lève std::invalid_argument si un départ tombe hors de la grille ou dans une zone qui ne brûle pas.
    SpreadResult run(const Weather& weather, std::span<const Ignition> ignitions, const SpreadOptions& options);

private:
    struct Conditions;
    SpreadVector cell_vector(size_t index, const Conditions& c) const;

    const Landscape& land_;
    std::vector<float> arrival_;
    std::vector<uint8_t> burned_;
    std::vector<uint32_t> touched_;  // cellules à remettre à zéro avant la prochaine simulation
};

}  // namespace firesim

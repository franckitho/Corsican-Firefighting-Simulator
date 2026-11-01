#pragma once

#include <cstdint>
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

struct SpreadResult {
    std::vector<BurnedCell> cells;  // dans l'ordre d'arrivée du feu
    double area_ha = 0;
    bool reached_grid_edge = false;
    int col_min = 0, row_min = 0, col_max = -1, row_max = -1;
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

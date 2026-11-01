#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "firesim/grid.hpp"
#include "firesim/wind_atlas.hpp"

namespace firesim {

// Couches nécessaires à la propagation, produites par le pipeline Python (étapes « export » et « wind »).
struct Landscape {
    GridInfo grid;
    std::vector<uint8_t> fuel;       // modèle Scott & Burgan ; 0 = mer
    std::vector<float> slope_deg;
    std::vector<float> aspect_deg;   // direction de descente (depuis le nord, sens horaire) ; -1 = plat
    std::vector<uint8_t> barrier_m;  // largeur de la coupure la plus large de la cellule
    std::optional<WindAtlas> wind;   // vent corrigé par le relief, si l'atlas a été calculé
    // Grille en Lambert 93 : le vent, donné par rapport au nord géographique, doit être tourné de la
    // convergence des méridiens pour s'accorder avec les pentes, calculées sur la grille.
    bool lambert93 = false;

    static Landscape load(const std::filesystem::path& dir);
    // Terrain homogène, pour les tests.
    static Landscape uniform(const GridInfo& grid, uint8_t fuel, float slope_deg = 0.f, float aspect_deg = -1.f);
};

}  // namespace firesim

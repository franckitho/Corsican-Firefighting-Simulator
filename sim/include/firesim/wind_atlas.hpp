#pragma once

#include <filesystem>
#include <vector>

#include "firesim/grid.hpp"

namespace firesim {

// Atlas des vents : pour N directions de vent général (0°, 360/N°…), le vent local à 10 m calculé par
// WindNinja pour un vent général de vitesse 1. Le relief accélère le vent sur les crêtes, le freine
// sous le vent et le canalise dans les vallées. Entre deux directions, on interpole.
class WindAtlas {
public:
    struct Local {
        double speed_factor;  // vitesse locale / vitesse du vent général
        double toward;        // azimut vers lequel souffle le vent local (radians)
    };

    static WindAtlas load(const std::filesystem::path& dir);
    // Atlas où le vent local est partout égal au vent général, multiplié par `factor` (tests).
    static WindAtlas uniform(const GridInfo& grid, int directions, double factor = 1.0);

    // Vent local au point (x, y) en Lambert 93 pour un vent général venant de `from_deg`.
    Local at(double x, double y, double from_deg) const;

    const GridInfo& grid() const { return grid_; }
    int directions() const { return directions_; }
    // Composantes est/nord du vent local pour la direction k, cellule (col, row).
    void set(int k, int col, int row, float east, float north);

private:
    GridInfo grid_;
    int directions_ = 0;
    std::vector<float> east_, north_;  // [direction][ligne][colonne]
};

}  // namespace firesim

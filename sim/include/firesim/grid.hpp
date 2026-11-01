#pragma once

#include <cstddef>
#include <optional>

namespace firesim {

// Grille régulière en Lambert 93 ; ligne 0 au nord, colonne 0 à l'ouest.
struct GridInfo {
    double left = 0, top = 0, res = 0;
    int width = 0, height = 0;

    size_t size() const { return static_cast<size_t>(width) * height; }
    size_t index(int col, int row) const { return static_cast<size_t>(row) * width + col; }
    bool contains(int col, int row) const { return col >= 0 && row >= 0 && col < width && row < height; }
    double x(int col) const { return left + (col + 0.5) * res; }  // centre de la cellule
    double y(int row) const { return top - (row + 0.5) * res; }

    struct Cell {
        int col, row;
    };
    std::optional<Cell> cell_at(double x, double y) const;
};

}  // namespace firesim

#include "firesim/landscape.hpp"

#include <cmath>
#include <stdexcept>

#include "firesim/io.hpp"

namespace firesim {

constexpr int kFormatVersion = 1;

std::optional<GridInfo::Cell> GridInfo::cell_at(double px, double py) const {
    const int col = static_cast<int>(std::floor((px - left) / res));
    const int row = static_cast<int>(std::floor((top - py) / res));
    if (!contains(col, row)) return std::nullopt;
    return Cell{col, row};
}

Landscape Landscape::load(const std::filesystem::path& dir) {
    const auto meta = io::read_flat_json(dir / "meta.json");
    if (static_cast<int>(io::json_number(meta, "format_version")) != kFormatVersion) {
        throw std::runtime_error("version de format inattendue dans " + (dir / "meta.json").string());
    }
    Landscape land;
    const auto crs = meta.find("crs");
    land.lambert93 = crs != meta.end() && crs->second == "EPSG:2154";
    land.grid.left = io::json_number(meta, "left");
    land.grid.top = io::json_number(meta, "top");
    land.grid.res = io::json_number(meta, "res");
    land.grid.width = static_cast<int>(io::json_number(meta, "width"));
    land.grid.height = static_cast<int>(io::json_number(meta, "height"));

    const size_t n = land.grid.size();
    land.fuel = io::read_raw<uint8_t>(dir / "fuel.u8", n);
    land.slope_deg = io::read_raw<float>(dir / "slope.f32", n);
    land.aspect_deg = io::read_raw<float>(dir / "aspect.f32", n);
    land.barrier_m = io::read_raw<uint8_t>(dir / "barrier.u8", n);
    if (std::filesystem::exists(dir / "wind" / "meta.json")) land.wind = WindAtlas::load(dir / "wind");
    return land;
}

Landscape Landscape::uniform(const GridInfo& grid, uint8_t fuel, float slope_deg, float aspect_deg) {
    Landscape land;
    land.grid = grid;
    land.fuel.assign(grid.size(), fuel);
    land.slope_deg.assign(grid.size(), slope_deg);
    land.aspect_deg.assign(grid.size(), aspect_deg);
    land.barrier_m.assign(grid.size(), 0);
    return land;
}

}  // namespace firesim

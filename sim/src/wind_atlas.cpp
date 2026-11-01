#include "firesim/wind_atlas.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "firesim/io.hpp"

namespace firesim {

constexpr int kAtlasFormatVersion = 1;
constexpr double kDeg = std::numbers::pi / 180.0;

WindAtlas WindAtlas::load(const std::filesystem::path& dir) {
    const auto meta = io::read_flat_json(dir / "meta.json");
    if (static_cast<int>(io::json_number(meta, "format_version")) != kAtlasFormatVersion) {
        throw std::runtime_error("version de format inattendue dans " + (dir / "meta.json").string());
    }
    WindAtlas atlas;
    atlas.grid_.left = io::json_number(meta, "left");
    atlas.grid_.top = io::json_number(meta, "top");
    atlas.grid_.res = io::json_number(meta, "res");
    atlas.grid_.width = static_cast<int>(io::json_number(meta, "width"));
    atlas.grid_.height = static_cast<int>(io::json_number(meta, "height"));
    atlas.directions_ = static_cast<int>(io::json_number(meta, "directions"));
    const size_t n = atlas.grid_.size() * atlas.directions_;
    atlas.east_ = io::read_raw<float>(dir / "east.f32", n);
    atlas.north_ = io::read_raw<float>(dir / "north.f32", n);
    return atlas;
}

WindAtlas WindAtlas::uniform(const GridInfo& grid, int directions, double factor) {
    WindAtlas atlas;
    atlas.grid_ = grid;
    atlas.directions_ = directions;
    atlas.east_.resize(grid.size() * directions);
    atlas.north_.resize(grid.size() * directions);
    for (int k = 0; k < directions; ++k) {
        const double toward = (k * 360.0 / directions + 180.0) * kDeg;
        for (int row = 0; row < grid.height; ++row) {
            for (int col = 0; col < grid.width; ++col) {
                atlas.set(k, col, row, static_cast<float>(factor * std::sin(toward)),
                          static_cast<float>(factor * std::cos(toward)));
            }
        }
    }
    return atlas;
}

void WindAtlas::set(int k, int col, int row, float east, float north) {
    const size_t i = static_cast<size_t>(k) * grid_.size() + grid_.index(col, row);
    east_[i] = east;
    north_[i] = north;
}

WindAtlas::Local WindAtlas::at(double x, double y, double from_deg) const {
    // Interpolation bilinéaire entre les centres de cellules, bornée aux bords de l'atlas.
    const double fx = std::clamp((x - grid_.left) / grid_.res - 0.5, 0.0, grid_.width - 1.0);
    const double fy = std::clamp((grid_.top - y) / grid_.res - 0.5, 0.0, grid_.height - 1.0);
    const int c0 = std::min(static_cast<int>(fx), grid_.width - 2 < 0 ? 0 : grid_.width - 2);
    const int r0 = std::min(static_cast<int>(fy), grid_.height - 2 < 0 ? 0 : grid_.height - 2);
    const int c1 = std::min(c0 + 1, grid_.width - 1), r1 = std::min(r0 + 1, grid_.height - 1);
    const double tx = fx - c0, ty = fy - r0;

    // Entre les deux directions encadrantes, on interpole la vitesse et la déviation imposée par le relief
    // (écart entre le vent local et le vent général), pas le vecteur lui-même : un atlas sans relief
    // redonne ainsi exactement le vent général, quelle que soit sa direction.
    const double step = 360.0 / directions_;
    const double f = std::fmod(std::fmod(from_deg, 360.0) + 360.0, 360.0) / step;
    const int k0 = static_cast<int>(f) % directions_;
    const int k1 = (k0 + 1) % directions_;
    const double w = f - std::floor(f);

    double speed = 0.0, dev_cos = 0.0, dev_sin = 0.0;
    for (const auto [k, weight] : {std::pair{k0, 1.0 - w}, std::pair{k1, w}}) {
        const size_t base = static_cast<size_t>(k) * grid_.size();
        auto sample = [&](const std::vector<float>& a) {
            const double top = a[base + grid_.index(c0, r0)] * (1 - tx) + a[base + grid_.index(c1, r0)] * tx;
            const double bottom = a[base + grid_.index(c0, r1)] * (1 - tx) + a[base + grid_.index(c1, r1)] * tx;
            return top * (1 - ty) + bottom * ty;
        };
        const double e = sample(east_), n = sample(north_);
        const double s = std::hypot(e, n);
        const double deviation = s > 1e-9 ? std::atan2(e, n) - (k * step + 180.0) * kDeg : 0.0;
        speed += weight * s;
        dev_cos += weight * std::cos(deviation);
        dev_sin += weight * std::sin(deviation);
    }
    const double toward = (from_deg + 180.0) * kDeg + std::atan2(dev_sin, dev_cos);
    return {speed, std::fmod(std::fmod(toward, 2 * std::numbers::pi) + 2 * std::numbers::pi, 2 * std::numbers::pi)};
}

}  // namespace firesim

#include <cmath>
#include <limits>
#include <numbers>

#include "check.hpp"
#include "firesim/spread.hpp"

using namespace firesim;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;
const Moisture kDry = *moisture_scenario("D1L1");

GridInfo square(int n, double res) { return {0.0, n * res, res, n, n}; }

}  // namespace

TEST_CASE(atlas_interpolates_between_directions) {
    const WindAtlas atlas = WindAtlas::uniform(square(4, 200), 8);
    const auto north = atlas.at(400, 400, 0);  // vent du nord : souffle vers le sud
    CHECK_NEAR(north.speed_factor, 1.0, 1e-6);
    CHECK_NEAR(north.toward / kDeg, 180.0, 1e-6);
    const auto between = atlas.at(400, 400, 22.5);  // entre nord (0°) et nord-est (45°)
    CHECK_NEAR(between.speed_factor, 1.0, 1e-6);    // pas d'affaiblissement entre deux directions
    CHECK_NEAR(between.toward / kDeg, 202.5, 1e-6);
    const auto wrap = atlas.at(400, 400, 350);      // entre 315° et 0°
    CHECK_NEAR(wrap.toward / kDeg, 170.0, 1e-6);
}

TEST_CASE(atlas_interpolates_in_space) {
    WindAtlas atlas = WindAtlas::uniform(square(2, 100), 4);
    for (int k = 0; k < 4; ++k) {
        for (int row = 0; row < 2; ++row) {
            atlas.set(k, 0, row, 0.f, -1.f);  // colonne ouest : facteur 1
            atlas.set(k, 1, row, 0.f, -3.f);  // colonne est : facteur 3
        }
    }
    CHECK_NEAR(atlas.at(100, 100, 0).speed_factor, 2.0, 1e-6);  // à mi-chemin des deux centres
    CHECK_NEAR(atlas.at(10, 100, 0).speed_factor, 1.0, 1e-6);   // au-delà des centres : valeur du bord
    CHECK_NEAR(atlas.at(190, 100, 0).speed_factor, 3.0, 1e-6);
}

TEST_CASE(uniform_atlas_changes_nothing) {
    const GridInfo g = square(121, 25);
    Landscape plain = Landscape::uniform(g, 102);
    Landscape with_atlas = Landscape::uniform(g, 102);
    with_atlas.wind = WindAtlas::uniform(square(16, 200), 16);
    const Ignition ignition{g.x(60), g.y(60)};
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 90;
    const Weather weather = Weather::constant(30, 300, kDry);  // direction entre deux directions de l'atlas
    Simulator a(plain), b(with_atlas);
    const auto ra = a.run(weather, {&ignition, 1}, options);
    const auto rb = b.run(weather, {&ignition, 1}, options);
    // Mêmes heures d'arrivée, aux arrondis près (l'atlas est stocké en float)
    std::vector<float> arrival(g.size(), std::numeric_limits<float>::infinity());
    for (const auto& c : rb.cells) arrival[c.index] = c.arrival_min;
    for (const auto& c : ra.cells) {
        if (c.arrival_min < options.duration_min - 0.01) CHECK_NEAR(arrival[c.index], c.arrival_min, 1e-4);
    }
}

TEST_CASE(stronger_local_wind_speeds_up_the_fire) {
    const GridInfo g = square(121, 25);
    Landscape land = Landscape::uniform(g, 102);
    // Atlas : vent local deux fois plus fort sur la moitié est de la carte
    WindAtlas atlas = WindAtlas::uniform(square(16, 200), 16);
    for (int k = 0; k < 16; ++k) {
        const double toward = (k * 22.5 + 180.0) * kDeg;
        for (int row = 0; row < 16; ++row) {
            for (int col = 8; col < 16; ++col) {
                atlas.set(k, col, row, static_cast<float>(2 * std::sin(toward)), static_cast<float>(2 * std::cos(toward)));
            }
        }
    }
    land.wind = atlas;
    const Ignition west{g.x(20), g.y(30)}, east{g.x(100), g.y(30)};
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 60;
    const Weather weather = Weather::constant(20, 0, kDry);  // vent du nord
    Simulator sim(land);
    const auto rw = sim.run(weather, {&west, 1}, options);
    const auto re = sim.run(weather, {&east, 1}, options);
    CHECK(re.area_ha > 1.5 * rw.area_ha);
    options.terrain_wind = false;  // l'atlas peut être ignoré
    const auto rw_flat = sim.run(weather, {&west, 1}, options);
    const auto re_flat = sim.run(weather, {&east, 1}, options);
    CHECK(rw_flat.cells.size() == re_flat.cells.size());
}

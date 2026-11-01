#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "check.hpp"
#include "firesim/spread.hpp"

using namespace firesim;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kRes = 25.0;
constexpr int kGrass = 102;  // GR2
const Moisture kDry = *moisture_scenario("D1L1");

GridInfo square(int n) { return {0.0, n * kRes, kRes, n, n}; }

std::vector<float> arrival_map(const SpreadResult& r, size_t size) {
    std::vector<float> a(size, std::numeric_limits<float>::infinity());
    for (const auto& c : r.cells) a[c.index] = c.arrival_min;
    return a;
}

// Vitesse (m/min) attendue depuis le point d'allumage dans la direction `azimuth_deg`.
double expected_ros(int fuel, double wind_kmh, double wind_from_deg, double slope_deg, double upslope_deg,
                    double azimuth_deg) {
    const FuelModel& model = *find_fuel_model(fuel);
    const FuelBed bed = fuel_bed(model, kDry);
    const double midflame = wind_kmh * kFtMinPerKmh * k10mTo20ft * wind_adjustment_factor(model.depth_ft);
    const SpreadVector v = spread_vector(bed, midflame, (wind_from_deg + 180.0) * kDeg, slope_deg, upslope_deg * kDeg);
    return v.ros_toward(azimuth_deg * kDeg) * kMetersPerFoot;
}

// Compare l'heure d'arrivée simulée à distance / vitesse attendue, à `cells` cellules dans la direction donnée.
void check_arrival(const std::vector<float>& arrival, const GridInfo& g, int c0, int r0, double azimuth_deg, int cells,
                   double ros_m_min, double tolerance) {
    const int col = c0 + static_cast<int>(std::lround(cells * std::sin(azimuth_deg * kDeg)));
    const int row = r0 - static_cast<int>(std::lround(cells * std::cos(azimuth_deg * kDeg)));
    const double distance = std::hypot(col - c0, row - r0) * kRes;
    CHECK_NEAR(arrival[g.index(col, row)], distance / ros_m_min, tolerance);
}

}  // namespace

TEST_CASE(without_wind_or_slope_fire_is_a_circle) {
    const GridInfo g = square(241);
    const Landscape land = Landscape::uniform(g, kGrass);
    const double ros = expected_ros(kGrass, 0, 0, 0, 0, 0);
    const Ignition ignition{g.x(120), g.y(120)};
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 110 * kRes / ros;
    Simulator sim(land);
    const auto arrival = arrival_map(sim.run(Weather::constant(0, 0, kDry), {&ignition, 1}, options), g.size());
    // Sur 16 voisins, l'erreur de distance reste sous ~3 % quelle que soit la direction.
    for (double az = 0; az < 360; az += 7.5) check_arrival(arrival, g, 120, 120, az, 100, ros, 0.03);
}

TEST_CASE(wind_stretches_fire_into_rothermel_ellipse) {
    const GridInfo g = square(241);
    const Landscape land = Landscape::uniform(g, kGrass);
    const double wind = 25, from = 270;  // vent d'ouest : le feu file vers l'est
    const Ignition ignition{g.x(60), g.y(120)};
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 170 * kRes / expected_ros(kGrass, wind, from, 0, 0, 90);
    Simulator sim(land);
    const auto arrival = arrival_map(sim.run(Weather::constant(wind, from, kDry), {&ignition, 1}, options), g.size());
    check_arrival(arrival, g, 60, 120, 90, 160, expected_ros(kGrass, wind, from, 0, 0, 90), 0.01);   // tête
    check_arrival(arrival, g, 60, 120, 0, 20, expected_ros(kGrass, wind, from, 0, 0, 0), 0.05);      // flanc
    check_arrival(arrival, g, 60, 120, 270, 3, expected_ros(kGrass, wind, from, 0, 0, 270), 0.05);   // arrière
}

TEST_CASE(fire_runs_faster_uphill) {
    const GridInfo g = square(161);
    const double slope = 20, aspect = 180;  // versant exposé au sud : on monte vers le nord
    const Landscape land = Landscape::uniform(g, kGrass, static_cast<float>(slope), static_cast<float>(aspect));
    const Ignition ignition{g.x(80), g.y(80)};
    const double up = expected_ros(kGrass, 0, 0, slope, 0, 0);
    const double down = expected_ros(kGrass, 0, 0, slope, 0, 180);
    CHECK(up > 2 * down);  // sans vent, la pente seule donne une ellipse peu allongée
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 75 * kRes / up;
    Simulator sim(land);
    const auto arrival = arrival_map(sim.run(Weather::constant(0, 0, kDry), {&ignition, 1}, options), g.size());
    check_arrival(arrival, g, 80, 80, 0, 70, up, 0.01);
    check_arrival(arrival, g, 80, 80, 180, 5, down, 0.05);
}

TEST_CASE(firebreak_holds_against_small_flames_but_not_large_ones) {
    const GridInfo g = square(121);
    const double wind = 30, from = 270;
    const FuelModel& model = *find_fuel_model(kGrass);
    const FuelBed bed = fuel_bed(model, kDry);
    const double head_flame = flame_length_ft(fireline_intensity(bed, expected_ros(kGrass, wind, from, 0, 0, 90) / kMetersPerFoot)) * kMetersPerFoot;
    const Ignition ignition{g.x(40), g.y(60)};
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 600;

    auto burned_beyond_road = [&](double road_width) {
        Landscape land = Landscape::uniform(g, kGrass);
        for (int row = 0; row < g.height; ++row) land.barrier_m[g.index(70, row)] = static_cast<uint8_t>(road_width);
        Simulator sim(land);
        const auto result = sim.run(Weather::constant(wind, from, kDry), {&ignition, 1}, options);
        return std::any_of(result.cells.begin(), result.cells.end(),
                           [&](const BurnedCell& c) { return static_cast<int>(c.index % g.width) > 70; });
    };
    CHECK(head_flame > 1.0);
    CHECK(!burned_beyond_road(std::ceil(options.breach_factor * head_flame) + 1));  // assez large : le feu s'arrête
    CHECK(burned_beyond_road(std::floor(head_flame)));                              // trop étroite : il passe
}

TEST_CASE(fire_does_not_slip_through_a_diagonal_line_of_water) {
    const GridInfo g = square(101);
    Landscape land = Landscape::uniform(g, 145);  // maquis, flammes de plusieurs mètres
    for (int i = 0; i < g.width; ++i) land.fuel[g.index(i, i)] = 98;
    const Ignition ignition{g.x(70), g.y(20)};  // côté col > row
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 2000;
    Simulator sim(land);
    const auto result = sim.run(Weather::constant(40, 45, kDry), {&ignition, 1}, options);  // vent poussant vers la ligne
    CHECK(result.cells.size() > 1000);
    for (const auto& c : result.cells) CHECK(static_cast<int>(c.index % g.width) > static_cast<int>(c.index / g.width));
}

TEST_CASE(simulator_can_be_reused) {
    const GridInfo g = square(81);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition a{g.x(20), g.y(40)}, b{g.x(60), g.y(40)};
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 120;
    Simulator sim(land);
    const Weather w = Weather::constant(15, 0, kDry);
    const auto first = sim.run(w, {&a, 1}, options);
    sim.run(w, {&b, 1}, options);
    const auto again = sim.run(w, {&a, 1}, options);
    CHECK(first.cells.size() == again.cells.size());
    CHECK(first.cells.back().arrival_min == again.cells.back().arrival_min);
}

TEST_CASE(ros_factor_slows_the_whole_fire) {
    const GridInfo g = square(81);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition ignition{g.x(40), g.y(40)};
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 1e6;
    Simulator sim(land);
    const Weather w = Weather::constant(20, 270, kDry);
    const auto normal = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    options.ros_factor = 0.5;
    const auto slow = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    for (const size_t i : {g.index(70, 40), g.index(10, 40), g.index(40, 5), g.index(65, 70)}) {
        CHECK_NEAR(slow[i], 2 * normal[i], 1e-5);
    }
}

TEST_CASE(swapped_fuel_burns_like_its_replacement) {
    const GridInfo g = square(41);
    const Ignition ignition{g.x(20), g.y(20)};
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 60;
    const Weather w = Weather::constant(20, 0, kDry);
    constexpr uint8_t kTimber = 165, kShrub = 145;  // TU5 (sous couvert), SH5
    const Landscape timber = Landscape::uniform(g, kTimber);
    const Landscape shrub = Landscape::uniform(g, kShrub);
    Simulator a(timber), b(shrub);
    options.fuel_swap = {{kTimber, kShrub}};
    const auto swapped = a.run(w, {&ignition, 1}, options);
    options.fuel_swap.clear();
    const auto reference = b.run(w, {&ignition, 1}, options);
    CHECK(swapped.cells.size() == reference.cells.size());
    CHECK(swapped.cells.back().arrival_min == reference.cells.back().arrival_min);
}

TEST_CASE(ignition_in_water_is_rejected) {
    const GridInfo g = square(11);
    Landscape land = Landscape::uniform(g, 98);
    const Ignition ignition{g.x(5), g.y(5)};
    Simulator sim(land);
    bool thrown = false;
    try {
        sim.run(Weather::constant(0, 0, kDry), {&ignition, 1}, SpreadOptions{});
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
}

TEST_CASE(weather_periods_and_moisture_scenarios) {
    const Weather w({{0, 10, 270, kDry}, {120, 40, 0, kDry}});
    CHECK(w.period_at(0) == 0);
    CHECK(w.period_at(119.9) == 0);
    CHECK(w.period_at(120) == 1);
    CHECK(w.period_at(1e6) == 1);
    const Moisture m = *moisture_scenario("D2L3");
    CHECK_NEAR(m.d1h, 0.06, 1e-12);
    CHECK_NEAR(m.d100h, 0.08, 1e-12);
    CHECK_NEAR(m.live_herb, 0.90, 1e-12);
    CHECK_NEAR(m.live_woody, 1.20, 1e-12);
    CHECK(!moisture_scenario("D5L1"));
    CHECK(moisture_scenario("extreme")->d1h == kDry.d1h);
}

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "check.hpp"
#include "firesim/suppression.hpp"

using namespace firesim;

namespace {

constexpr double kRes = 25.0;
constexpr uint8_t kGrass = 102, kShrub = 145;
const Moisture kDry = *moisture_scenario("D1L1");
// Une carte placée sur la Corse : les bases des avions sont à leur vraie distance.
constexpr double kLeft = 1215000.0, kTop = 6170000.0;

GridInfo square(int n) { return {kLeft, kTop, kRes, n, n}; }
Lambert93 at(const GridInfo& g, int col, int row) { return {g.x(col), g.y(row)}; }

std::vector<float> arrival_map(const SpreadResult& r, size_t size) {
    std::vector<float> a(size, std::numeric_limits<float>::infinity());
    for (const auto& c : r.cells) a[c.index] = c.arrival_min;
    return a;
}

bool burned_east_of(const SpreadResult& r, const GridInfo& g, int col) {
    return std::any_of(r.cells.begin(), r.cells.end(),
                       [&](const BurnedCell& c) { return static_cast<int>(c.index % g.width) > col; });
}

// Vent d'ouest, feu parti de l'ouest, ligne nord-sud sur toute la carte à la colonne `col`, construite à partir de `built_at`.
std::vector<Treatment> north_south_line(const GridInfo& g, int col, float built_at, float max_flame, int32_t line = 0) {
    std::vector<Treatment> t;
    for (int row = 0; row < g.height; ++row) {
        t.push_back({static_cast<uint32_t>(g.index(col, row)), built_at, std::numeric_limits<float>::infinity(), max_flame, line});
    }
    return t;
}

SpreadOptions fire_options() {
    SpreadOptions options;
    options.ros_factor = 1.0;  // Rothermel seul
    options.duration_min = 600;
    return options;
}

}  // namespace

TEST_CASE(no_treatment_gives_the_same_fire) {
    const GridInfo g = square(81);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition ignition{g.x(40), g.y(40)};
    const Weather w = Weather::constant(25, 270, kDry);
    SpreadOptions options = fire_options();
    Simulator sim(land);
    const auto reference = sim.run(w, {&ignition, 1}, options);
    // Une bande loin du feu ne change rien non plus
    options.treatments = {{0u, 0.f, 1.f, 100.f, -1}};
    const auto again = sim.run(w, {&ignition, 1}, options);
    CHECK(again.cells.size() == reference.cells.size());
    CHECK(again.cells.back().arrival_min == reference.cells.back().arrival_min);
}

TEST_CASE(a_finished_line_stops_the_fire_but_not_if_the_fire_gets_there_first) {
    const GridInfo g = square(121);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition ignition{g.x(40), g.y(60)};
    const Weather w = Weather::constant(30, 270, kDry);
    SpreadOptions options = fire_options();
    Simulator sim(land);

    const auto free_fire = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    const float reaches_line = free_fire[g.index(70, 60)];
    CHECK(std::isfinite(reaches_line));

    options.treatments = north_south_line(g, 70, reaches_line / 2, 100.f);
    const auto held = sim.run(w, {&ignition, 1}, options);
    CHECK(!burned_east_of(held, g, 70));
    CHECK(!std::isfinite(held.lines[0].breach_min));

    options.treatments = north_south_line(g, 70, reaches_line * 2, 100.f);
    const auto late = sim.run(w, {&ignition, 1}, options);
    CHECK(burned_east_of(late, g, 70));
    CHECK(std::isfinite(late.lines[0].cut_min));
}

TEST_CASE(a_line_too_narrow_for_the_flames_is_breached) {
    const GridInfo g = square(121);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition ignition{g.x(40), g.y(60)};
    SpreadOptions options = fire_options();
    options.treatments = north_south_line(g, 70, 0.f, 0.1f);
    Simulator sim(land);
    const auto result = sim.run(Weather::constant(30, 270, kDry), {&ignition, 1}, options);
    CHECK(burned_east_of(result, g, 70));
    CHECK(std::isfinite(result.lines[0].breach_min));
    CHECK(!std::isfinite(result.lines[0].cut_min));
}

TEST_CASE(fire_reaching_the_worksite_stops_the_rest_of_the_line) {
    const GridInfo g = square(121);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Weather w = Weather::constant(0, 0, kDry);
    // Chantier du nord vers le sud, très lent ; le feu, parti tout près du milieu de la ligne, y arrive avant lui.
    const Ignition ignition{g.x(55), g.y(60)};
    SpreadOptions options = fire_options();
    for (int row = 0; row < g.height; ++row) {
        options.treatments.push_back({static_cast<uint32_t>(g.index(60, row)), 10.f * row,
                                      std::numeric_limits<float>::infinity(), 100.f, 0});
    }
    Simulator sim(land);
    const auto result = sim.run(w, {&ignition, 1}, options);
    const LineOutcome& line = result.lines[0];
    CHECK(std::isfinite(line.cut_min));
    CHECK(line.cut_from > 0.f);
    CHECK(line.cut_from < 600.f);
}

TEST_CASE(a_drop_delays_the_fire_then_it_resumes) {
    const GridInfo g = square(121);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition ignition{g.x(40), g.y(60)};
    const Weather w = Weather::constant(30, 270, kDry);
    SpreadOptions options = fire_options();
    Simulator sim(land);
    const auto free_fire = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    const float reaches = free_fire[g.index(70, 60)];

    constexpr float kHold = 60.f;
    for (int row = 0; row < g.height; ++row) {
        options.treatments.push_back({static_cast<uint32_t>(g.index(70, row)), 0.f, reaches + kHold, 100.f, -1});
    }
    const auto delayed = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    // Le feu repart à la fin de l'effet : il franchit la bande un peu après, et plus aucune cellule n'est en avance.
    CHECK(delayed[g.index(70, 60)] >= reaches + kHold);
    CHECK(delayed[g.index(70, 60)] < reaches + kHold + 5.f);
    CHECK(delayed[g.index(100, 60)] > free_fire[g.index(100, 60)] + kHold / 2);

    // Un retardant posé longtemps avant le feu le retient quand même, à partir du contact
    options.treatments.clear();
    for (int row = 0; row < g.height; ++row) {
        options.treatments.push_back({static_cast<uint32_t>(g.index(70, row)), 0.f, 1e6f, 100.f, -1, kHold});
    }
    const auto retardant = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    CHECK_NEAR(retardant[g.index(70, 60)], reaches + kHold, 1.0);

    // Des flammes trop hautes pour le largage : le feu passe comme s'il n'était pas là
    for (Treatment& t : options.treatments) t.max_flame_m = 0.1f;
    const auto through = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    CHECK_NEAR(through[g.index(100, 60)], free_fire[g.index(100, 60)], 1e-3);
}

TEST_CASE(a_drop_that_has_evaporated_does_nothing) {
    const GridInfo g = square(121);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition ignition{g.x(40), g.y(60)};
    const Weather w = Weather::constant(30, 270, kDry);
    SpreadOptions options = fire_options();
    Simulator sim(land);
    const auto free_fire = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    const float reaches = free_fire[g.index(70, 60)];
    CHECK(reaches > 2.f);
    for (int row = 0; row < g.height; ++row) {
        options.treatments.push_back({static_cast<uint32_t>(g.index(70, row)), 0.f, reaches / 2, 100.f, -1});
    }
    const auto dried = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    CHECK_NEAR(dried[g.index(100, 60)], free_fire[g.index(100, 60)], 1e-3);
}

TEST_CASE(a_drop_on_the_burning_edge_delays_what_it_was_lighting) {
    const GridInfo g = square(121);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition ignition{g.x(40), g.y(60)};
    const Weather w = Weather::constant(30, 270, kDry);
    SpreadOptions options = fire_options();
    Simulator sim(land);
    const auto free_fire = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    // De l'eau sur toute la colonne 60, juste après que le feu y arrive
    constexpr float kWet = 20.f;
    for (int row = 0; row < g.height; ++row) {
        const float burned = free_fire[g.index(60, row)];
        if (!std::isfinite(burned)) continue;
        options.treatments.push_back({static_cast<uint32_t>(g.index(60, row)), burned + 0.001f, burned + 0.001f + kWet, 100.f, -1});
    }
    const auto doused = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    CHECK_NEAR(doused[g.index(60, 60)], free_fire[g.index(60, 60)], 1e-3);  // la cellule elle-même a déjà pris
    CHECK(doused[g.index(61, 60)] > free_fire[g.index(60, 60)] + kWet - 1.f);
}

TEST_CASE(a_line_finished_while_the_fire_is_on_its_way_holds) {
    const GridInfo g = square(121);
    const Landscape land = Landscape::uniform(g, kGrass);
    const Ignition ignition{g.x(40), g.y(60)};
    const Weather w = Weather::constant(30, 270, kDry);
    SpreadOptions options = fire_options();
    Simulator sim(land);
    const auto free_fire = arrival_map(sim.run(w, {&ignition, 1}, options), g.size());
    // Le feu arrive à la ligne juste après sa fin de chantier
    const float reaches = free_fire[g.index(70, 60)];
    const float neighbour = free_fire[g.index(69, 60)];
    const float built_at = neighbour + (reaches - neighbour) * 0.5f;
    options.treatments = north_south_line(g, 70, built_at, 100.f);
    const auto result = sim.run(w, {&ignition, 1}, options);
    CHECK(!std::isfinite(result.lines[0].breach_min));
}

TEST_CASE(worksite_speed_follows_engines_width_fuel_and_slope) {
    const GridInfo g = square(400);
    const Landscape shrub = Landscape::uniform(g, kShrub);
    FirebreakOrder order;
    order.path = {at(g, 10, 10), at(g, 210, 10)};  // 5 km
    order.order_min = 30;
    order.width_m = 4;
    const Firebreak one = plan_firebreak(shrub, order, 1.5);
    CHECK_NEAR(one.length_m, 5000.0, 1.0);
    CHECK_NEAR(one.start_min, 90.0, 1e-9);                                 // 1 h de transport
    CHECK_NEAR(one.finish_min - one.start_min, 5000.0 / 335.3 * 60.0, 1.0);  // maquis, 1 100 pieds/h
    CHECK_NEAR(one.holds_flame_m, 4.0 / 1.5, 1e-9);
    CHECK(one.built_min.front() <= one.built_min.back());

    order.width_m = 8;  // deux passes
    const Firebreak wide = plan_firebreak(shrub, order, 1.5);
    CHECK_NEAR(wide.finish_min - wide.start_min, 2 * (one.finish_min - one.start_min), 1e-3);
    order.engines = 2;
    const Firebreak two = plan_firebreak(shrub, order, 1.5);
    CHECK_NEAR(two.finish_min - two.start_min, one.finish_min - one.start_min, 1e-3);

    order.width_m = 4;
    order.engines = 1;
    const Firebreak grass = plan_firebreak(Landscape::uniform(g, kGrass), order, 1.5);
    CHECK(grass.finish_min - grass.start_min < one.finish_min - one.start_min);
    const Firebreak steep = plan_firebreak(Landscape::uniform(g, kShrub, 35.f, 90.f), order, 1.5);  // 70 % de pente
    CHECK(steep.finish_min - steep.start_min > 2 * (one.finish_min - one.start_min));
}

TEST_CASE(worksite_line_has_no_hole) {
    const GridInfo g = square(121);
    const Landscape land = Landscape::uniform(g, kGrass);
    FirebreakOrder order;
    order.path = {at(g, 60, 0), at(g, 75, 40), at(g, 55, 90), at(g, 60, 120)};
    order.width_m = 40;
    const Firebreak line = plan_firebreak(land, order, 1.5);
    SpreadOptions options = fire_options();
    options.duration_min = 2000;
    for (size_t i = 0; i < line.cells.size(); ++i) {
        options.treatments.push_back({line.cells[i], 0.f, std::numeric_limits<float>::infinity(), 100.f, 0});
    }
    const Ignition ignition{g.x(20), g.y(60)};
    Simulator sim(land);
    const auto result = sim.run(Weather::constant(30, 270, kDry), {&ignition, 1}, options);
    CHECK(result.cells.size() > 500);
    CHECK(!burned_east_of(result, g, 75));
}

TEST_CASE(firebreak_outside_the_map_is_rejected) {
    const GridInfo g = square(21);
    const Landscape land = Landscape::uniform(g, kGrass);
    FirebreakOrder order;
    order.path = {at(g, 5, 5), {kLeft - 5000, kTop}};
    bool thrown = false;
    try {
        plan_firebreak(land, order, 1.5);
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
}

TEST_CASE(canadair_leaves_ajaccio_scoops_then_drops) {
    const GridInfo g = square(200);
    const Landscape land = Landscape::uniform(g, kShrub);
    const DropOrder order{AircraftKind::Canadair, at(g, 50, 100), at(g, 60, 100), 30.0};
    const std::vector<Flight> flights = plan_air_attack(land, {&order, 1}, std::nullopt);
    CHECK(flights.size() == 1);
    const Flight& f = flights[0];
    CHECK(f.base == "Ajaccio");
    CHECK(f.callsign == "Pélican 1");
    CHECK(f.origin == "Ajaccio");
    CHECK(f.first_order_min == 30.0);
    CHECK(f.drops.size() == 1);
    CHECK(std::any_of(f.track.begin(), f.track.end(), [](const Waypoint& w) { return w.phase == FlightPhase::Scoop; }));
    // Ordre, alerte de 15 min, puis vol ; rien ne se passe avant
    CHECK(f.at(30.0).phase == FlightPhase::Parked);
    CHECK(f.at(44.9).phase == FlightPhase::Parked);
    CHECK(f.at(45.1).phase == FlightPhase::Climb);
    CHECK(f.drops[0].start_min > 45.0);
    // Il rentre à sa base
    const Lambert93 ajaccio = to_lambert93({41.9236, 8.8029});
    CHECK_NEAR(f.track.back().position.x, ajaccio.x, 1.0);
    CHECK(f.track.back().phase == FlightPhase::Parked);
}

TEST_CASE(a_long_axis_takes_several_passes) {
    const GridInfo g = square(200);
    const Landscape land = Landscape::uniform(g, kShrub);
    const DropOrder order{AircraftKind::Canadair, at(g, 50, 100), at(g, 90, 100), 0.0};  // 1 km : 330 m par passage
    CHECK(drop_passes(order) == 4);
    const std::vector<Flight> flights = plan_air_attack(land, {&order, 1}, std::nullopt);
    CHECK(flights.size() == 1);
    CHECK(flights[0].drops.size() == 4);
    for (size_t i = 1; i < flights[0].drops.size(); ++i) {
        CHECK(flights[0].drops[i].start_min > flights[0].drops[i - 1].end_min);
    }
}

TEST_CASE(canadair_rotation_on_the_coast_takes_about_eight_minutes) {
    // Tout le sud de la carte est de la mer, à 500 m du largage
    const GridInfo g = square(800);
    Landscape land = Landscape::uniform(g, kShrub);
    for (int row = 420; row < g.height; ++row) {
        for (int col = 0; col < g.width; ++col) land.fuel[g.index(col, row)] = 0;
    }
    const DropOrder order{AircraftKind::Canadair, at(g, 300, 400), at(g, 330, 400), 0.0};
    const DropOrder second{AircraftKind::Canadair, at(g, 330, 400), at(g, 360, 400), 0.0};
    const DropOrder axis{AircraftKind::Canadair, order.from, second.to, 0.0};
    const std::vector<Flight> flights = plan_air_attack(land, {&axis, 1}, std::nullopt);
    CHECK(flights.size() == 1);
    CHECK(flights[0].drops.size() >= 2);
    const double rotation = flights[0].drops[1].start_min - flights[0].drops[0].start_min;
    CHECK(rotation > 3.0);
    CHECK(rotation < 8.0);
}

TEST_CASE(dash_comes_from_nimes_and_reloads_at_the_nearest_pelicandrome) {
    const GridInfo g = square(200);
    const Landscape land = Landscape::uniform(g, kShrub);
    const DropOrder order{AircraftKind::Dash, at(g, 50, 100), at(g, 130, 100), 0.0};  // 2 km : 1 km par passage
    const std::vector<Flight> flights = plan_air_attack(land, {&order, 1}, std::nullopt);
    CHECK(flights.size() == 1);
    const Flight& f = flights[0];
    CHECK(f.callsign == "Milan 1");
    CHECK(f.drops.size() == 2);
    // Nîmes - Corse (environ 400 km) à 560 km/h : plus de 40 min de vol, sans écopage
    CHECK(f.drops[0].start_min > 15.0 + 40.0);
    CHECK(std::none_of(f.track.begin(), f.track.end(), [](const Waypoint& w) { return w.phase == FlightPhase::Scoop; }));
    CHECK(std::any_of(f.track.begin(), f.track.end(), [](const Waypoint& w) { return w.phase == FlightPhase::Reload; }));
    CHECK(f.origin == "Nîmes-Garons");
    CHECK(f.base != "Nîmes-Garons");
    const double rotation = f.drops[1].start_min - f.drops[0].start_min;
    CHECK(rotation > 20.0);
    CHECK(rotation < 45.0);
}

TEST_CASE(orders_go_to_the_plane_that_drops_first) {
    const GridInfo g = square(200);
    const Landscape land = Landscape::uniform(g, kShrub);
    const DropOrder a{AircraftKind::Canadair, at(g, 50, 100), at(g, 60, 100), 0.0};
    const std::vector<DropOrder> orders = {a, a, a};
    const std::vector<Flight> flights = plan_air_attack(land, orders, std::nullopt);
    // Deux avions à Ajaccio, puis le premier qui se libère plutôt qu'un avion venu de Nîmes
    CHECK(flights.size() == 2);
    CHECK(flights[0].base == "Ajaccio");
    CHECK(flights[1].base == "Ajaccio");
    CHECK(flights[0].drops.size() + flights[1].drops.size() == 3);
}

TEST_CASE(a_plane_called_back_turns_around_without_landing) {
    const GridInfo g = square(200);
    const Landscape land = Landscape::uniform(g, kShrub);
    const DropOrder a{AircraftKind::Canadair, at(g, 50, 100), at(g, 60, 100), 0.0};
    const std::vector<Flight> once = plan_air_attack(land, {&a, 1}, std::nullopt);
    const double end_of_drop = once[0].drops[0].end_min;
    // Un second ordre juste après le largage, un troisième pour occuper le second avion
    const DropOrder b{AircraftKind::Canadair, at(g, 50, 100), at(g, 60, 100), end_of_drop + 3.0};
    const DropOrder busy{AircraftKind::Canadair, at(g, 150, 100), at(g, 160, 100), 0.0};
    const std::vector<DropOrder> orders = {a, busy, b};
    const std::vector<Flight> flights = plan_air_attack(land, orders, std::nullopt);
    const auto with_two = std::find_if(flights.begin(), flights.end(), [](const Flight& f) { return f.drops.size() == 2; });
    CHECK(with_two != flights.end());
    if (with_two != flights.end()) {
        const double between = with_two->drops[1].start_min - with_two->drops[0].end_min;
        CHECK(between < 40.0);
        CHECK(std::is_sorted(with_two->track.begin(), with_two->track.end(),
                             [](const Waypoint& x, const Waypoint& y) { return x.time_min < y.time_min; }));
    }
}

TEST_CASE(sun_times_in_corsica) {
    const LatLon ajaccio{41.92, 8.80};
    // Solstice d'été à Ajaccio (heure d'été) : lever vers 5 h 55, coucher vers 21 h 10.
    const Daylight summer(172, 0.0, 2.0, ajaccio);
    CHECK_NEAR(summer.sunrise_clock_min(0), 5 * 60 + 55, 10.0);
    CHECK_NEAR(summer.sunset_clock_min(0), 21 * 60 + 10, 10.0);
    // Solstice d'hiver (heure d'hiver) : lever vers 8 h 15, coucher vers 17 h 15.
    const Daylight winter(355, 0.0, 1.0, ajaccio);
    CHECK_NEAR(winter.sunrise_clock_min(0), 8 * 60 + 15, 10.0);
    CHECK_NEAR(winter.sunset_clock_min(0), 17 * 60 + 15, 10.0);
}

TEST_CASE(no_flying_at_night) {
    const GridInfo g = square(200);
    const Landscape land = Landscape::uniform(g, kShrub);
    const DropOrder order{AircraftKind::Canadair, at(g, 50, 100), at(g, 60, 100), 0.0};
    const LatLon ajaccio{41.92, 8.80};

    // Feu à 14 h : largage l'après-midi même
    const Daylight afternoon(200, 14 * 60.0, 2.0, ajaccio);
    const std::vector<Flight> day = plan_air_attack(land, {&order, 1}, afternoon);
    CHECK(day[0].drops[0].start_min < 120.0);

    // Feu à 23 h : personne ne décolle avant le lever du soleil, 7 h plus tard environ
    const Daylight night(200, 23 * 60.0, 2.0, ajaccio);
    const std::vector<Flight> late = plan_air_attack(land, {&order, 1}, night);
    CHECK(late[0].drops[0].start_min > night.next_sunrise(0.0));
    CHECK_NEAR(night.next_sunrise(0.0), 24 * 60 + night.sunrise_clock_min(1) - 23 * 60, 1e-6);
    CHECK(night.allows(late[0].drops[0].start_min, late[0].drops[0].end_min));
}

TEST_CASE(air_treatments_follow_the_strip) {
    const GridInfo g = square(200);
    const Landscape land = Landscape::uniform(g, kShrub);
    const DropOrder order{AircraftKind::Dash, at(g, 50, 100), at(g, 60, 100), 0.0};
    const std::vector<Flight> flights = plan_air_attack(land, {&order, 1}, std::nullopt);
    const std::vector<Treatment> treatments = treatments_of(g, {}, flights);
    CHECK(treatments.size() >= 30);  // un largage entier : 1 km = 40 cellules
    const AircraftType& dash = aircraft_type(AircraftKind::Dash);
    for (const Treatment& t : treatments) {
        CHECK(t.line == -1);
        CHECK_NEAR(t.until_min - t.from_min, dash.lasting_min, 1e-2);
        CHECK_NEAR(t.hold_min, dash.hold_min, 1e-3);
        CHECK(t.from_min >= flights[0].drops[0].start_min - 1e-3);
        CHECK(t.from_min <= flights[0].drops[0].end_min + 1e-3);
    }
    // 2 000 kW/m : flammes de 2 à 3 m
    CHECK(treatments[0].max_flame_m > 2.0f);
    CHECK(treatments[0].max_flame_m < 3.5f);
}

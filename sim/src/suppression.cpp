#include "firesim/suppression.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "firesim/rothermel.hpp"

namespace firesim {
namespace {

constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kFeet = kMetersPerFoot;

// Bulldozer en une passe, lame de 12 à 14 pieds, pente moyenne de 40 % (guides américains, 3 000 / 1 500 / 1 100 pieds/h).
constexpr double kBladeWidthM = 4.0;
constexpr double kGrassMPerH = 3000 * kFeet, kBrushMPerH = 1500 * kFeet, kHeavyBrushMPerH = 1100 * kFeet;
// Au-delà de 55 % de pente, un bulldozer ne monte plus : le chantier avance à la vitesse d'une équipe à pied (471 pieds/h).
constexpr double kMaxDozerSlopePct = 55.0;
constexpr double kFootCrewMPerH = 471 * kFeet;
// Hypothèse, sans source : transport des engins jusqu'au premier point du tracé.
constexpr double kEngineArrivalMin = 60.0;

// Hypothèse, sans source : de l'ordre au décollage d'un avion au sol.
constexpr double kAlertMin = 15.0;
// Longueurs des trajectoires autour d'un largage : le total reproduit les rotations annoncées (8 min en bord de mer
// pour le Canadair, 25 min pour le Dash).
constexpr double kClimbM = 8000.0, kApproachM = 4000.0, kExitM = 4000.0, kOffshoreM = 1000.0;
constexpr double kClimbSpeedFactor = 0.7;
// Écopage : 410 m à 130 km/h, soit 12 s.
constexpr double kScoopM = 410.0, kScoopKmh = 130.0;
// Sur un pélicandrome : 10 min de remplissage, et le roulage (hypothèse, pour retrouver la rotation annoncée).
constexpr double kReloadMin = 16.0;

constexpr double kMaxScoopSearchM = 300000.0;

struct Airfield {
    const char* name;
    LatLon where;
};
const Airfield kAjaccio{"Ajaccio", {41.9236, 8.8029}};
const Airfield kNimes{"Nîmes-Garons", {43.7574, 4.4164}};
// Pélicandromes permanents de Corse
const Airfield kPelicandromes[] = {kAjaccio, {"Bastia-Poretta", {42.5527, 9.4837}}, {"Calvi", {42.5244, 8.7932}},
                                   {"Solenzara", {41.9244, 9.4060}}};

// Deux Canadair détachés à Ajaccio pour la saison ; à Nîmes, le reste de ce qui vole en plein été (8 à 10 sur 12).
// Les huit Dash sont tous à Nîmes.
struct Stationed {
    AircraftKind kind;
    const Airfield* base;
    int count;
};
const Stationed kFleet[] = {{AircraftKind::Canadair, &kAjaccio, 2},
                            {AircraftKind::Canadair, &kNimes, 6},
                            {AircraftKind::Dash, &kNimes, 8}};

double m_per_min(double kmh) { return kmh * 1000.0 / 60.0; }

Lambert93 lambert(const LatLon& p) { return to_lambert93(p); }

double distance(const Lambert93& a, const Lambert93& b) { return std::hypot(b.x - a.x, b.y - a.y); }

Lambert93 toward(const Lambert93& from, const Lambert93& to, double meters) {
    const double d = distance(from, to);
    if (d <= 0) return from;
    return {from.x + (to.x - from.x) / d * meters, from.y + (to.y - from.y) / d * meters};
}

double flame_length_m(double intensity_kw_m) {
    return flame_length_ft(intensity_kw_m / kKwPerMPerBtuFtS) * kMetersPerFoot;
}

// Vitesse d'un bulldozer en une passe.
double engine_speed_m_h(uint8_t fuel, float slope_deg) {
    if (std::isfinite(slope_deg) && std::tan(slope_deg * kDeg) * 100.0 > kMaxDozerSlopePct) return kFootCrewMPerH;
    if (fuel >= 101 && fuel <= 109) return kGrassMPerH;                                     // herbe
    if ((fuel >= 141 && fuel <= 149) || (fuel >= 161 && fuel <= 165)) return kHeavyBrushMPerH;  // maquis, sous-bois
    return kBrushMPerH;
}

struct GridPoint {
    int col, row;
};

GridPoint grid_point(const GridInfo& g, const Lambert93& p) {
    return {static_cast<int>(std::floor((p.x - g.left) / g.res)), static_cast<int>(std::floor((g.top - p.y) / g.res))};
}

// Cellules d'un segment, reliées par côté ou par coin (Bresenham), le dernier point compris.
template <class Visit>
void walk_segment(GridPoint a, const GridPoint b, Visit visit) {
    const int dc = std::abs(b.col - a.col), dr = -std::abs(b.row - a.row);
    const int sc = a.col < b.col ? 1 : -1, sr = a.row < b.row ? 1 : -1;
    int error = dc + dr;
    while (true) {
        visit(a);
        if (a.col == b.col && a.row == b.row) break;
        const int e2 = 2 * error;
        if (e2 >= dr) {
            error += dr;
            a.col += sc;
        }
        if (e2 <= dc) {
            error += dc;
            a.row += sr;
        }
    }
}

bool is_sea(const Landscape& land, const Lambert93& p) {
    const auto cell = land.grid.cell_at(p.x, p.y);
    return !cell || land.fuel[land.grid.index(cell->col, cell->row)] == 0;  // hors de la carte : la mer
}

struct ScoopRun {
    Lambert93 start, end;
};

// Mer la plus proche du point : on cherche par cercles croissants un endroit dégagé, à distance de la côte, et l'avion
// écope parallèlement à la côte.
ScoopRun nearest_scoop_run(const Landscape& land, const Lambert93& target) {
    const double step = std::max(250.0, land.grid.res);
    constexpr int kBearings = 36;
    for (double radius = step; radius < kMaxScoopSearchM; radius += step) {
        for (int i = 0; i < kBearings; ++i) {
            const double bearing = i * 2.0 * std::numbers::pi / kBearings;
            const double ux = std::sin(bearing), uy = std::cos(bearing);
            const Lambert93 middle{target.x + ux * (radius + kOffshoreM), target.y + uy * (radius + kOffshoreM)};
            const Lambert93 start{middle.x - uy * kScoopM / 2, middle.y + ux * kScoopM / 2};
            const Lambert93 end{middle.x + uy * kScoopM / 2, middle.y - ux * kScoopM / 2};
            const Lambert93 shore{target.x + ux * radius, target.y + uy * radius};
            if (is_sea(land, shore) && is_sea(land, middle) && is_sea(land, start) && is_sea(land, end)) {
                return {start, end};
            }
        }
    }
    throw std::invalid_argument("pas de mer à portée d'un Canadair");
}

const Airfield& nearest_pelicandrome(const Lambert93& p) {
    const Airfield* best = &kPelicandromes[0];
    for (const Airfield& field : kPelicandromes) {
        if (distance(p, lambert(field.where)) < distance(p, lambert(best->where))) best = &field;
    }
    return *best;
}

struct Strip {
    Lambert93 from, to;
};

// Découpe un axe en bandes de la longueur d'un largage ; un axe plus court reçoit un largage entier, centré.
std::vector<Strip> strips_of(const DropOrder& order) {
    const double strip = aircraft_type(order.kind).strip_m;
    const double length = distance(order.from, order.to);
    const int passes = drop_passes(order);
    std::vector<Strip> strips;
    if (length <= strip) {
        const Lambert93 middle{(order.from.x + order.to.x) / 2, (order.from.y + order.to.y) / 2};
        const double ux = length > 0 ? (order.to.x - order.from.x) / length : 0.0;
        const double uy = length > 0 ? (order.to.y - order.from.y) / length : 1.0;
        strips.push_back({{middle.x - ux * strip / 2, middle.y - uy * strip / 2},
                          {middle.x + ux * strip / 2, middle.y + uy * strip / 2}});
        return strips;
    }
    for (int i = 0; i < passes; ++i) {
        strips.push_back({toward(order.from, order.to, length * i / passes),
                          toward(order.from, order.to, length * (i + 1) / passes)});
    }
    return strips;
}

// Un avion et ce qui lui reste à faire : on construit sa trajectoire tronçon par tronçon.
class Plane {
public:
    Plane(std::string callsign, AircraftKind kind, const Airfield& base, const Landscape& land)
        : land_(&land), type_(&aircraft_type(kind)), home_(lambert(base.where)) {
        flight.callsign = std::move(callsign);
        flight.origin = base.name;
        flight.base = base.name;
        flight.kind = kind;
        flight.track.push_back({0.0, home_, FlightPhase::Parked});
    }

    // Prochain largage de cet avion pour cette bande, sans rien modifier : sert à choisir l'avion.
    double first_drop_after(double order_min, const Strip& strip, const std::optional<Daylight>& daylight) const {
        Plane trial = *this;
        trial.begin_mission(order_min);
        trial.rotation(strip, 0, daylight);
        return trial.flight.drops.back().start_min;
    }

    void fly_mission(double order_min, int order, const std::vector<Strip>& strips,
                     const std::optional<Daylight>& daylight) {
        if (flight.drops.empty()) flight.first_order_min = order_min;
        begin_mission(order_min);
        for (const Strip& strip : strips) rotation(strip, order, daylight);
        end_mission();
    }

    Flight flight;

private:
    double now() const { return flight.track.back().time_min; }
    const Lambert93& here() const { return flight.track.back().position; }

    void leg(const Lambert93& to, double speed_m_min, FlightPhase phase) {
        flight.track.back().phase = phase;
        flight.track.push_back({now() + distance(here(), to) / speed_m_min, to, FlightPhase::Parked});
    }

    void wait(double minutes, FlightPhase phase) {
        flight.track.back().phase = phase;
        flight.track.push_back({now() + minutes, here(), FlightPhase::Parked});
    }

    void fly_to(const Lambert93& to) {
        if (!airborne_) {
            leg(toward(here(), to, kClimbM), m_per_min(type_->cruise_kmh) * kClimbSpeedFactor, FlightPhase::Climb);
            airborne_ = true;
        }
        leg(to, m_per_min(type_->cruise_kmh), FlightPhase::Cruise);
    }

    void land_at(const Lambert93& field) {
        const double remaining = distance(here(), field);
        if (remaining > kClimbM) leg(toward(here(), field, remaining - kClimbM), m_per_min(type_->cruise_kmh), FlightPhase::Cruise);
        leg(field, m_per_min(type_->cruise_kmh) * kClimbSpeedFactor, FlightPhase::Descent);
        airborne_ = false;
    }

    // L'ordre arrive : l'avion part quand il a fini, au sol après l'alerte, en vol depuis le point où il se trouve.
    void begin_mission(double order_min) {
        const double start = std::max(order_min, free_min_);
        if (start >= now()) {
            wait(start - now(), FlightPhase::Parked);
            wait(kAlertMin, FlightPhase::Parked);
            return;
        }
        const Lambert93 position = flight.at(start).position;
        while (flight.track.back().time_min > start) flight.track.pop_back();
        flight.track.back().phase = FlightPhase::Cruise;
        flight.track.push_back({start, position, FlightPhase::Parked});
        airborne_ = true;
    }

    void end_mission() {
        free_min_ = now();
        if (flight.kind == AircraftKind::Canadair) land_at(home_);  // peut être interrompu par l'ordre suivant
    }

    void go_to_bed(const std::optional<Daylight>& daylight) {
        if (airborne_) land_at(home_);
        wait(std::max(0.0, daylight->next_sunrise(now()) - now()), FlightPhase::Parked);
    }

    void reload() {
        const Airfield& field = nearest_pelicandrome(here());
        home_ = lambert(field.where);
        land_at(home_);
        flight.base = field.name;
        wait(kReloadMin, FlightPhase::Reload);
        free_min_ = now();
    }

    void drop(const Strip& strip, int order) {
        if (flight.kind == AircraftKind::Canadair) {
            const ScoopRun run = nearest_scoop_run(*land_, strip.from);
            fly_to(run.start);
            leg(run.end, m_per_min(kScoopKmh), FlightPhase::Scoop);
        }
        const Lambert93 approach = toward(strip.from, strip.to, -kApproachM);
        fly_to(approach);
        const double drop_speed = m_per_min(type_->drop_kmh);
        leg(strip.from, drop_speed, FlightPhase::Approach);
        const double start = now();
        leg(strip.to, drop_speed, FlightPhase::Drop);
        flight.drops.push_back({start, now(), strip.from, strip.to, order});
        leg(toward(strip.from, strip.to, distance(strip.from, strip.to) + kExitM), drop_speed, FlightPhase::Exit);
        if (flight.kind == AircraftKind::Dash) reload();
    }

    // Un passage complet ; reporté au lendemain s'il ne peut pas finir avant la nuit.
    void rotation(const Strip& strip, int order, const std::optional<Daylight>& daylight) {
        if (daylight) {
            Plane trial = *this;
            trial.drop(strip, order);
            if (!daylight->allows(now(), trial.flight.drops.back().end_min)) go_to_bed(daylight);
        }
        drop(strip, order);
    }

    const Landscape* land_;
    const AircraftType* type_;
    Lambert93 home_;
    bool airborne_ = false;
    double free_min_ = 0;
};

}  // namespace

Firebreak plan_firebreak(const Landscape& land, const FirebreakOrder& order, double breach_factor) {
    if (order.path.size() < 2) throw std::invalid_argument("un pare-feu a besoin d'au moins deux points");
    const GridInfo& g = land.grid;
    Firebreak line;
    line.order = order;
    line.start_min = order.order_min + kEngineArrivalMin;
    line.holds_flame_m = order.width_m / breach_factor;
    const double passes = std::max(1.0, std::ceil(order.width_m / kBladeWidthM));
    const double engines = std::max(1, order.engines);

    double time = line.start_min;
    std::optional<GridPoint> previous;
    auto visit = [&](const GridPoint p) {
        if (previous && previous->col == p.col && previous->row == p.row) return;
        if (!g.contains(p.col, p.row)) throw std::invalid_argument("pare-feu hors de la carte");
        const double step = previous ? std::hypot(p.col - previous->col, p.row - previous->row) * g.res : 0.0;
        const size_t i = g.index(p.col, p.row);
        const double speed = engine_speed_m_h(land.fuel[i], land.slope_deg[i]) / 60.0 * engines / passes;
        line.length_m += step;
        time += step / speed;
        line.cells.push_back(static_cast<uint32_t>(i));
        line.built_min.push_back(static_cast<float>(time));
        previous = p;
    };
    for (size_t i = 0; i + 1 < order.path.size(); ++i) {
        walk_segment(grid_point(g, order.path[i]), grid_point(g, order.path[i + 1]), visit);
    }
    line.finish_min = time;
    return line;
}

const AircraftType& aircraft_type(AircraftKind kind) {
    // Canadair CL-415 : croisière 290 km/h, 6 137 L d'eau en séquence sur 330 m, de 30 m de haut.
    // Dash 8 Q400MR : croisière 560 km/h, largage à 230 km/h, de 40 m de haut, sur près d'un kilomètre.
    // Le retardant reste actif une fois sec (jusqu'à une semaine) et retient jusqu'à une heure un feu sous 2 000 kW/m
    // (Project Aquarius). L'eau : 20 min, hypothèse sans source ; elle s'évapore et ne laisse rien derrière elle.
    constexpr double kUntilEvaporated = std::numeric_limits<double>::infinity();
    static const AircraftType kCanadair{"Canadair", "Pélican", "eau", 290.0, 200.0, 30.0, 330.0, 20.0, kUntilEvaporated, 2000.0};
    static const AircraftType kDash{"Dash", "Milan", "retardant", 560.0, 230.0, 40.0, 1000.0, 7 * 1440.0, 60.0, 2000.0};
    return kind == AircraftKind::Canadair ? kCanadair : kDash;
}

int drop_passes(const DropOrder& order) {
    return std::max(1, static_cast<int>(std::ceil(distance(order.from, order.to) / aircraft_type(order.kind).strip_m)));
}

Flight::State Flight::at(double time_min) const {
    if (track.empty()) return {};
    if (time_min <= track.front().time_min) return {track.front().position, track.front().phase};
    for (size_t i = 0; i + 1 < track.size(); ++i) {
        const Waypoint& a = track[i];
        const Waypoint& b = track[i + 1];
        if (time_min < b.time_min) {
            const double f = (time_min - a.time_min) / (b.time_min - a.time_min);
            return {{a.position.x + (b.position.x - a.position.x) * f, a.position.y + (b.position.y - a.position.y) * f},
                    a.phase};
        }
    }
    return {track.back().position, track.back().phase};
}

Daylight::Daylight(int day_of_year, double start_clock_min, double utc_offset_h, const LatLon& where)
    : day_of_year_(day_of_year), start_clock_min_(start_clock_min), utc_offset_h_(utc_offset_h), where_(where) {}

// Durée d'une demi-journée, soleil à 0,833° sous l'horizon à son lever (réfraction, rayon du disque).
double Daylight::half_day_min(int day) const {
    const double n = day_of_year_ + day;
    const double declination = 23.44 * kDeg * std::sin(2.0 * std::numbers::pi * (284.0 + n) / 365.0);
    const double lat = where_.lat * kDeg;
    const double cos_h = (std::sin(-0.833 * kDeg) - std::sin(lat) * std::sin(declination)) / (std::cos(lat) * std::cos(declination));
    return std::acos(std::clamp(cos_h, -1.0, 1.0)) / kDeg * 4.0;
}

double Daylight::solar_noon_min(int day) const {
    const double b = 2.0 * std::numbers::pi * (day_of_year_ + day - 81.0) / 364.0;
    const double equation_of_time = 9.87 * std::sin(2.0 * b) - 7.53 * std::cos(b) - 1.5 * std::sin(b);
    return 720.0 + utc_offset_h_ * 60.0 - 4.0 * where_.lon - equation_of_time;
}

double Daylight::sunrise_clock_min(int day) const { return solar_noon_min(day) - half_day_min(day); }
double Daylight::sunset_clock_min(int day) const { return solar_noon_min(day) + half_day_min(day); }

bool Daylight::allows(double t0, double t1) const {
    const double clock0 = start_clock_min_ + t0;
    const int day = static_cast<int>(std::floor(clock0 / 1440.0));
    return clock0 - day * 1440.0 >= sunrise_clock_min(day) && start_clock_min_ + t1 - day * 1440.0 <= sunset_clock_min(day);
}

double Daylight::next_sunrise(double t) const {
    const double clock = start_clock_min_ + t;
    int day = static_cast<int>(std::floor(clock / 1440.0));
    if (clock - day * 1440.0 >= sunrise_clock_min(day)) ++day;
    return day * 1440.0 + sunrise_clock_min(day) - start_clock_min_;
}

std::vector<Flight> plan_air_attack(const Landscape& land, std::span<const DropOrder> orders,
                                    const std::optional<Daylight>& daylight) {
    std::vector<Plane> planes;
    int numbers[2] = {0, 0};
    for (const Stationed& group : kFleet) {
        for (int i = 0; i < group.count; ++i) {
            const int number = ++numbers[static_cast<int>(group.kind)];
            planes.emplace_back(std::string(aircraft_type(group.kind).radio) + " " + std::to_string(number), group.kind,
                                *group.base, land);
        }
    }

    std::vector<int> sequence(orders.size());
    for (size_t i = 0; i < orders.size(); ++i) sequence[i] = static_cast<int>(i);
    std::stable_sort(sequence.begin(), sequence.end(),
                     [&](int a, int b) { return orders[a].order_min < orders[b].order_min; });
    for (const int i : sequence) {
        const DropOrder& order = orders[i];
        const std::vector<Strip> strips = strips_of(order);
        Plane* best = nullptr;
        double best_time = std::numeric_limits<double>::infinity();
        for (Plane& plane : planes) {
            if (plane.flight.kind != order.kind) continue;
            const double time = plane.first_drop_after(order.order_min, strips.front(), daylight);
            if (time < best_time) {
                best_time = time;
                best = &plane;
            }
        }
        best->fly_mission(order.order_min, i, strips, daylight);
    }

    std::vector<Flight> flights;
    for (const Plane& plane : planes) {
        if (!plane.flight.drops.empty()) flights.push_back(plane.flight);
    }
    return flights;
}

std::vector<uint32_t> cells_along(const GridInfo& grid, std::span<const Lambert93> path) {
    std::vector<uint32_t> cells;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        walk_segment(grid_point(grid, path[i]), grid_point(grid, path[i + 1]), [&](const GridPoint p) {
            if (!grid.contains(p.col, p.row)) return;
            const uint32_t cell = static_cast<uint32_t>(grid.index(p.col, p.row));
            if (cells.empty() || cells.back() != cell) cells.push_back(cell);
        });
    }
    return cells;
}

std::vector<Treatment> treatments_of(const GridInfo& grid, std::span<const Firebreak> firebreaks,
                                     std::span<const Flight> flights) {
    constexpr float kForever = std::numeric_limits<float>::infinity();
    std::vector<Treatment> treatments;
    for (size_t line = 0; line < firebreaks.size(); ++line) {
        const Firebreak& f = firebreaks[line];
        for (size_t i = 0; i < f.cells.size(); ++i) {
            treatments.push_back({f.cells[i], f.built_min[i], kForever, static_cast<float>(f.holds_flame_m), static_cast<int32_t>(line)});
        }
    }
    for (const Flight& flight : flights) {
        const AircraftType& type = aircraft_type(flight.kind);
        const float max_flame = static_cast<float>(flame_length_m(type.max_intensity_kw_m));
        for (const AirDrop& drop : flight.drops) {
            const Lambert93 axis[] = {drop.from, drop.to};
            const std::vector<uint32_t> cells = cells_along(grid, axis);
            for (size_t i = 0; i < cells.size(); ++i) {
                const double f = static_cast<double>(i) / std::max<size_t>(1, cells.size() - 1);
                const float from = static_cast<float>(drop.start_min + (drop.end_min - drop.start_min) * f);
                treatments.push_back({cells[i], from, from + static_cast<float>(type.lasting_min), max_flame, -1,
                                      static_cast<float>(type.hold_min)});
            }
        }
    }
    return treatments;
}

}  // namespace firesim

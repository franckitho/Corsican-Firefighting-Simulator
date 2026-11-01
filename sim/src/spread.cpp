#include "firesim/spread.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <queue>
#include <stdexcept>

#include "firesim/lambert93.hpp"

namespace firesim {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kNever = std::numeric_limits<double>::infinity();

// Pas vers l'une des 16 cellules voisines. `via` : pour une diagonale, les deux cellules « coins » ;
// pour un pas de cavalier (2, 1), les deux cellules que le segment traverse en chemin.
struct Step {
    int dc, dr;
    double length;   // en cellules
    double azimuth;  // radians depuis le nord, sens horaire
    bool diagonal, knight;
    std::array<std::array<int, 2>, 2> via;
};

std::array<Step, 16> make_steps() {
    std::array<Step, 16> steps{};
    size_t n = 0;
    for (int dr = -2; dr <= 2; ++dr) {
        for (int dc = -2; dc <= 2; ++dc) {
            const int adc = std::abs(dc), adr = std::abs(dr);
            const bool orthogonal = adc + adr == 1;
            const bool diagonal = adc == 1 && adr == 1;
            const bool knight = adc + adr == 3 && adc > 0 && adr > 0;
            if (!orthogonal && !diagonal && !knight) continue;
            Step s{dc, dr, std::hypot(dc, dr), std::atan2(dc, -dr), diagonal, knight, {}};
            if (diagonal) s.via = {{{dc, 0}, {0, dr}}};
            if (knight) s.via = adc == 2 ? std::array<std::array<int, 2>, 2>{{{dc / 2, 0}, {dc / 2, dr}}}
                                         : std::array<std::array<int, 2>, 2>{{{0, dr / 2}, {dc, dr / 2}}};
            steps[n++] = s;
        }
    }
    return steps;
}

const std::array<Step, 16> kSteps = make_steps();

struct Node {
    float time;
    uint32_t index;
    bool operator>(const Node& o) const { return time > o.time; }
};

// La lutte, rangée par cellule, et ce que le feu fait aux chantiers de pare-feu pendant le calcul.
class ActiveTreatments {
public:
    explicit ActiveTreatments(std::span<const Treatment> treatments) : sorted_(treatments.begin(), treatments.end()) {
        std::stable_sort(sorted_.begin(), sorted_.end(), [](const Treatment& a, const Treatment& b) {
            return a.cell != b.cell ? a.cell < b.cell : a.from_min < b.from_min;
        });
        int32_t lines = 0;
        for (const Treatment& t : sorted_) lines = std::max(lines, t.line + 1);
        lines_.resize(lines);
    }

    bool empty() const { return sorted_.empty(); }

    // Heure à laquelle un feu qui se présente à l'heure `time`, avec ces flammes, entre dans la cellule ; par heure de
    // début, car un largage peut en prolonger un autre.
    double free_at(size_t cell, double time, double flame_m) const {
        const auto [first, last] = of(cell);
        for (auto t = first; t != last; ++t) {
            if (flame_m > t->max_flame_m || (t->line >= 0 && t->from_min >= lines_[t->line].cut_from)) continue;
            if (t->from_min <= time && time < t->until_min) time = std::min<double>(t->until_min, time + t->hold_min);
        }
        return time;
    }

    // Un largage sur une cellule qui brûle retarde ce qu'elle était en train d'allumer (`arrival`).
    double doused_until(size_t cell, double burned_at, double arrival, double flame_m) const {
        const auto [first, last] = of(cell);
        for (auto t = first; t != last; ++t) {
            if (t->line >= 0 || flame_m > t->max_flame_m || t->from_min < burned_at || t->from_min >= arrival) continue;
            arrival = std::max<double>(arrival, std::min<double>(t->until_min, static_cast<double>(t->from_min) + t->hold_min));
        }
        return arrival;
    }

    void on_burned(size_t cell, float time) {
        if (empty()) return;
        const auto [first, last] = of(cell);
        for (auto t = first; t != last; ++t) {
            if (t->line < 0) continue;
            LineOutcome& line = lines_[t->line];
            if (time < t->from_min) {
                line.cut_min = std::min(line.cut_min, time);
                line.cut_from = std::min(line.cut_from, t->from_min);
            } else if (t->from_min < line.cut_from) {
                line.breach_min = std::min(line.breach_min, time);
            }
        }
    }

    const std::vector<LineOutcome>& lines() const { return lines_; }

private:
    std::pair<std::vector<Treatment>::const_iterator, std::vector<Treatment>::const_iterator> of(size_t cell) const {
        const auto first = std::lower_bound(sorted_.begin(), sorted_.end(), cell,
                                            [](const Treatment& t, size_t c) { return t.cell < c; });
        auto last = first;
        while (last != sorted_.end() && last->cell == cell) ++last;
        return {first, last};
    }

    std::vector<Treatment> sorted_;
    std::vector<LineOutcome> lines_;
};

}  // namespace

// Ce qui ne dépend que de la période météo : un lit de combustible et un facteur de réduction du vent
// par modèle, et le vent général.
struct Simulator::Conditions {
    std::array<FuelBed, 256> beds{};
    std::array<double, 256> waf{};  // vent à mi-flamme / vent à 20 ft
    double wind_20ft = 0;           // ft/min
    double wind_from_deg = 0;
    double wind_toward = 0;         // radians
    double ros_factor = 1;
    bool wind_limit = false;
    const WindAtlas* atlas = nullptr;

    void prepare(const WeatherPeriod& w, const SpreadOptions& options, const WindAtlas* wind_atlas) {
        wind_20ft = w.wind_kmh * kFtMinPerKmh * k10mTo20ft * options.wind_factor;
        wind_from_deg = w.wind_from_deg;
        wind_toward = std::fmod(w.wind_from_deg + 180.0, 360.0) * kDeg;
        ros_factor = options.ros_factor;
        wind_limit = options.wind_limit;
        atlas = options.terrain_wind ? wind_atlas : nullptr;
        std::array<int, 256> burns_as{};
        for (int n = 0; n < 256; ++n) burns_as[n] = n;
        for (const auto [from, to] : options.fuel_swap) burns_as[from] = to;
        for (int n = 0; n < 256; ++n) {
            const int m = burns_as[n];
            const FuelModel* model = find_fuel_model(m);
            beds[n] = model ? fuel_bed(*model, w.moisture) : FuelBed{};
            const Canopy* canopy = options.canopy && is_under_canopy(m) ? &*options.canopy : nullptr;
            waf[n] = model ? wind_adjustment_factor(model->depth_ft, canopy) : 0.0;
        }
    }
};

Simulator::Simulator(const Landscape& land)
    : land_(land), arrival_(land.grid.size(), kInf), burned_(land.grid.size(), 0) {}

SpreadVector Simulator::cell_vector(size_t i, const Conditions& c) const {
    const uint8_t fuel = land_.fuel[i];
    const FuelBed& bed = c.beds[fuel];
    if (!bed.burns()) return {};
    const float slope = std::isfinite(land_.slope_deg[i]) ? land_.slope_deg[i] : 0.f;
    const float aspect = land_.aspect_deg[i];
    const double upslope = (aspect >= 0.f && std::isfinite(aspect)) ? std::fmod(aspect + 180.0, 360.0) * kDeg : 0.0;
    double wind = c.wind_20ft, toward = c.wind_toward;
    if (wind > 0.0 && (c.atlas != nullptr || land_.lambert93)) {
        const GridInfo& g = land_.grid;
        const double x = g.x(static_cast<int>(i % g.width)), y = g.y(static_cast<int>(i / g.width));
        if (c.atlas != nullptr) {
            // L'atlas est déjà exprimé sur la grille (WindNinja tient compte de la convergence).
            const auto local = c.atlas->at(x, y, c.wind_from_deg);
            wind *= local.speed_factor;
            toward = local.toward;
        } else {
            toward -= meridian_convergence({x, y});
        }
    }
    SpreadVector v = spread_vector(bed, wind * c.waf[fuel], toward, slope, upslope, c.wind_limit);
    v.ros *= c.ros_factor;
    return v;
}

SpreadResult Simulator::run(const Weather& weather, std::span<const Ignition> ignitions, const SpreadOptions& options) {
    const GridInfo& g = land_.grid;
    for (const uint32_t i : touched_) {
        arrival_[i] = kInf;
        burned_[i] = 0;
    }
    touched_.clear();

    std::priority_queue<Node, std::vector<Node>, std::greater<>> queue;
    auto relax = [&](size_t i, float t) {
        if (t >= arrival_[i]) return;
        if (arrival_[i] == kInf) touched_.push_back(static_cast<uint32_t>(i));
        arrival_[i] = t;
        queue.push({t, static_cast<uint32_t>(i)});
    };

    for (const Ignition& ignition : ignitions) {
        const auto cell = g.cell_at(ignition.x, ignition.y);
        if (!cell) throw std::invalid_argument("départ de feu hors de la carte");
        const size_t i = g.index(cell->col, cell->row);
        if (!is_burnable(land_.fuel[i])) {
            throw std::invalid_argument("départ de feu dans une zone qui ne brûle pas (eau, roche, ville…)");
        }
        relax(i, static_cast<float>(ignition.time_min));
    }

    ActiveTreatments treatments(options.treatments);
    SpreadResult result;
    result.col_min = g.width;
    result.row_min = g.height;
    Conditions cond;
    size_t period = std::numeric_limits<size_t>::max();
    const double cell_ft = g.res / kMetersPerFoot;

    while (!queue.empty()) {
        const Node node = queue.top();
        queue.pop();
        if (burned_[node.index] || node.time != arrival_[node.index]) continue;  // entrée périmée
        if (node.time > options.duration_min) break;
        burned_[node.index] = 1;
        treatments.on_burned(node.index, node.time);

        if (const size_t p = weather.period_at(node.time); p != period) {
            cond.prepare(weather[p], options, land_.wind ? &*land_.wind : nullptr);
            period = p;
        }

        const int col = static_cast<int>(node.index % g.width);
        const int row = static_cast<int>(node.index / g.width);
        const FuelBed& bed = cond.beds[land_.fuel[node.index]];
        const SpreadVector here = cell_vector(node.index, cond);
        const double head_intensity = fireline_intensity(bed, here.ros);
        result.cells.push_back({node.index, node.time, static_cast<float>(here.ros * kMetersPerFoot),
                                static_cast<float>(flame_length_ft(head_intensity) * kMetersPerFoot),
                                static_cast<float>(head_intensity * kKwPerMPerBtuFtS),
                                static_cast<float>(here.direction / kDeg)});
        result.col_min = std::min(result.col_min, col);
        result.col_max = std::max(result.col_max, col);
        result.row_min = std::min(result.row_min, row);
        result.row_max = std::max(result.row_max, row);
        if (col == 0 || row == 0 || col == g.width - 1 || row == g.height - 1) result.reached_grid_edge = true;

        for (const Step& s : kSteps) {
            const int c = col + s.dc, r = row + s.dr;
            if (!g.contains(c, r)) continue;
            const size_t target = g.index(c, r);
            if (burned_[target]) continue;

            const double ros_out = here.ros_toward(s.azimuth);
            if (ros_out <= 1e-9) continue;
            // Une cellule arrête le feu venu d'ici si elle ne brûle pas, ou si sa coupure est
            // assez large pour la longueur de flamme dans cette direction.
            const double flame_m = flame_length_ft(fireline_intensity(bed, ros_out)) * kMetersPerFoot;
            auto stops = [&](size_t k) {
                return !is_burnable(land_.fuel[k]) ||
                       (land_.barrier_m[k] > 0 && land_.barrier_m[k] >= options.breach_factor * flame_m);
            };
            if (stops(target)) continue;

            auto ros_in = [&](size_t k) { return cell_vector(k, cond).ros_toward(s.azimuth); };
            // Temps de parcours : moitié du trajet dans chaque cellule, ou quarts pour un pas de cavalier.
            double inverse_sum = 1.0 / ros_out;
            double weight = 0.5;
            if (s.diagonal || s.knight) {
                const size_t a = g.index(col + s.via[0][0], row + s.via[0][1]);
                const size_t b = g.index(col + s.via[1][0], row + s.via[1][1]);
                if (s.diagonal && stops(a) && stops(b)) continue;  // ne pas se faufiler entre deux coins
                if (s.knight) {
                    if (stops(a) || stops(b)) continue;
                    const double ra = ros_in(a), rb = ros_in(b);
                    if (ra <= 1e-9 || rb <= 1e-9) continue;
                    inverse_sum += 1.0 / ra + 1.0 / rb;
                    weight = 0.25;
                }
            }
            const double rv = ros_in(target);
            if (rv <= 1e-9) continue;
            inverse_sum += 1.0 / rv;
            double arrival = node.time + s.length * cell_ft * weight * inverse_sum;
            if (!treatments.empty()) {
                arrival = treatments.doused_until(node.index, node.time, arrival, flame_m);
                // La lutte le retient à son arrivée : mêmes règles de passage, mais pour un temps
                double free = treatments.free_at(target, arrival, flame_m);
                if (s.diagonal || s.knight) {
                    const size_t a = g.index(col + s.via[0][0], row + s.via[0][1]);
                    const size_t b = g.index(col + s.via[1][0], row + s.via[1][1]);
                    const double free_a = stops(a) ? kNever : treatments.free_at(a, arrival, flame_m);
                    const double free_b = stops(b) ? kNever : treatments.free_at(b, arrival, flame_m);
                    free = std::max(free, s.diagonal ? std::min(free_a, free_b) : std::max(free_a, free_b));
                }
                if (free == kNever) continue;
                arrival = free;
            }
            relax(target, static_cast<float>(arrival));
        }
    }

    result.area_ha = static_cast<double>(result.cells.size()) * g.res * g.res / 10000.0;
    result.lines = treatments.lines();
    return result;
}

}  // namespace firesim

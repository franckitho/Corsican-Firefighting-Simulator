#include "firesim/weather.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace firesim {

namespace {

// Végétaux morts trempés : point de saturation des fibres.
constexpr double kSaturation = 0.35;
constexpr double kMinMoisture = 0.02;
// Pluie de l'heure à partir de laquelle les végétaux morts se mouillent.
constexpr double kWettingRainMm = 0.2;
// Correction du NFDRS de 1978 au ras du sol en plein soleil (ciel dégagé vers midi) : +25 °F et humidité de l'air
// x 0,75. Interpolée selon le rayonnement, elle retrouve les corrections du NFDRS par ciel nuageux.
constexpr double kFullSunWm2 = 900.0;
constexpr double kFullSunHeatingC = 25.0 * 5.0 / 9.0;
constexpr double kFullSunDrying = 0.25;
// Temps de réponse des végétaux morts (heures) : brindilles, branches, grosses branches.
constexpr double kTimeLagH[3] = {1.0, 10.0, 100.0};

// Découpe une ligne de CSV (virgules ou points-virgules) ; vide pour un commentaire ou un en-tête.
std::vector<std::string> data_cells(std::string line) {
    if (line.empty() || line[0] == '#' || !(std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '.')) {
        return {};
    }
    std::replace(line.begin(), line.end(), ';', ',');
    std::stringstream ss(line);
    std::vector<std::string> cells;
    for (std::string cell; std::getline(ss, cell, ',');) cells.push_back(cell);
    return cells;
}

}  // namespace

double equilibrium_moisture(double temperature_c, double humidity_pct) {
    const double t = temperature_c * 9.0 / 5.0 + 32.0;  // formules en °F
    const double h = std::clamp(humidity_pct, 0.0, 100.0);
    double emc_pct;
    if (h < 10) {
        emc_pct = 0.03229 + 0.281073 * h - 0.000578 * h * t;
    } else if (h < 50) {
        emc_pct = 2.22749 + 0.160107 * h - 0.01478 * t;
    } else {
        emc_pct = 21.0606 + 0.005565 * h * h - 0.00035 * h * t - 0.483199 * h;
    }
    return std::max(emc_pct, 0.0) / 100.0;
}

std::vector<DeadMoisture> dead_fuel_moisture(std::span<const WeatherHour> hours) {
    std::vector<DeadMoisture> out;
    out.reserve(hours.size());
    double m[3] = {};
    for (size_t i = 0; i < hours.size(); ++i) {
        const WeatherHour& w = hours[i];
        double target = kSaturation;
        if (w.rain_mm < kWettingRainMm) {
            const double sun = std::clamp(w.radiation_wm2 / kFullSunWm2, 0.0, 1.0);
            target = std::clamp(equilibrium_moisture(w.temperature_c + sun * kFullSunHeatingC,
                                                     w.humidity_pct * (1.0 - sun * kFullSunDrying)),
                                kMinMoisture, kSaturation);
        }
        for (int k = 0; k < 3; ++k) {
            m[k] = i == 0 ? target : target + (m[k] - target) * std::exp(-1.0 / kTimeLagH[k]);
        }
        out.push_back({m[0], m[1], m[2]});
    }
    return out;
}

size_t HourlyWeather::index_of(std::string_view time) const {
    const auto it = std::find(times.begin(), times.end(), time);
    if (it == times.end()) throw std::invalid_argument("heure absente des relevés météo : " + std::string(time));
    return static_cast<size_t>(std::distance(times.begin(), it));
}

HourlyWeather HourlyWeather::from_csv(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("impossible d'ouvrir " + path.string());
    HourlyWeather weather;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const std::vector<std::string> cells = data_cells(line);
        if (cells.empty()) continue;
        if (cells.size() != 7) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_no) + " : 7 colonnes attendues");
        }
        weather.times.push_back(cells[0]);
        weather.hours.push_back({std::stod(cells[1]), std::stod(cells[2]), std::stod(cells[3]), std::stod(cells[4]),
                                 std::stod(cells[5]), std::stod(cells[6])});
    }
    if (weather.hours.empty()) throw std::runtime_error(path.string() + " : aucun relevé");
    return weather;
}

Weather::Weather(std::vector<WeatherPeriod> periods) : periods_(std::move(periods)) {
    if (periods_.empty()) throw std::invalid_argument("météo vide");
    std::stable_sort(periods_.begin(), periods_.end(),
                     [](const auto& a, const auto& b) { return a.start_min < b.start_min; });
    periods_.front().start_min = 0.0;  // les premières conditions valent dès le départ du feu
}

Weather Weather::constant(double wind_kmh, double wind_from_deg, const Moisture& moisture) {
    return Weather({WeatherPeriod{0.0, wind_kmh, wind_from_deg, moisture}});
}

Weather Weather::from_csv(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("impossible d'ouvrir " + path.string());
    std::vector<WeatherPeriod> periods;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const std::vector<std::string> cells = data_cells(line);
        if (cells.empty()) continue;
        if (cells.size() != 8) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_no) + " : 8 colonnes attendues");
        }
        std::vector<double> v;
        for (const auto& cell : cells) v.push_back(std::stod(cell));
        periods.push_back({v[0] * 60.0, v[1], v[2], {v[3] / 100, v[4] / 100, v[5] / 100, v[6] / 100, v[7] / 100}});
    }
    return Weather(std::move(periods));
}

Weather Weather::from_hours(std::span<const WeatherHour> hours, size_t ignition_hour, double live_herb, double live_woody) {
    if (ignition_hour >= hours.size()) throw std::invalid_argument("heure de départ du feu hors des relevés météo");
    const std::vector<DeadMoisture> dead = dead_fuel_moisture(hours);
    std::vector<WeatherPeriod> periods;
    for (size_t i = ignition_hour; i < hours.size(); ++i) {
        periods.push_back({static_cast<double>(i - ignition_hour) * 60.0, hours[i].wind_kmh, hours[i].wind_from_deg,
                           {dead[i].d1h, dead[i].d10h, dead[i].d100h, live_herb, live_woody}});
    }
    return Weather(std::move(periods));
}

size_t Weather::period_at(double t_min) const {
    const auto it = std::upper_bound(periods_.begin(), periods_.end(), t_min,
                                     [](double t, const WeatherPeriod& p) { return t < p.start_min; });
    return static_cast<size_t>(std::distance(periods_.begin(), it)) - 1;
}

std::optional<Moisture> moisture_scenario(std::string_view name) {
    if (name == "extreme") name = "D1L1";
    if (name == "tres_sec") name = "D2L2";
    if (name == "sec") name = "D3L3";
    if (name == "modere") name = "D4L4";
    if (name.size() != 4 || name[0] != 'D' || name[2] != 'L') return std::nullopt;
    const int d = name[1] - '0', l = name[3] - '0';
    if (d < 1 || d > 4 || l < 1 || l > 4) return std::nullopt;
    // Morts : 3-4-5 %, 6-7-8 %, 9-10-11 %, 12-13-14 % ; vivants : herbe 30 à 120 %, ligneux 60 à 150 %.
    const double dead = 3.0 * d;
    const double herb = 30.0 * l;
    return Moisture{dead / 100, (dead + 1) / 100, (dead + 2) / 100, herb / 100, (herb + 30) / 100};
}

}  // namespace firesim

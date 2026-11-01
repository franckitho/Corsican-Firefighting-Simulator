#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "firesim/rothermel.hpp"

namespace firesim {

// Conditions constantes à partir de start_min (minutes depuis le début de la simulation).
struct WeatherPeriod {
    double start_min = 0;
    double wind_kmh = 0;       // vent moyen à 10 m
    double wind_from_deg = 0;  // direction d'où vient le vent (convention météo : 315 = mistral de nord-ouest)
    Moisture moisture{};
};

// Météo réelle d'une heure (prévision ou archive), au point de départ du feu.
struct WeatherHour {
    double wind_kmh = 0;       // vent moyen à 10 m
    double wind_from_deg = 0;
    double temperature_c = 20;  // à 2 m
    double humidity_pct = 50;   // humidité relative à 2 m
    double radiation_wm2 = 0;   // rayonnement solaire reçu au sol
    double rain_mm = 0;         // pluie tombée dans l'heure
};

// Relevés horaires consécutifs, avec leur date et heure locales (« 2025-07-20T13:00 »).
struct HourlyWeather {
    std::vector<std::string> times;
    std::vector<WeatherHour> hours;

    // Rang de l'heure `time` ; lève std::invalid_argument si elle n'y est pas.
    size_t index_of(std::string_view time) const;
    // CSV : date_heure, vent_kmh, vent_de_deg, temperature_c, humidite_pct, rayonnement_wm2, pluie_mm.
    static HourlyWeather from_csv(const std::filesystem::path& path);
};

// Humidité des végétaux morts, en fraction de leur masse sèche.
struct DeadMoisture {
    double d1h, d10h, d100h;
};

// Humidité d'équilibre des végétaux morts avec l'air (fraction), d'après Simard (1968), comme le NFDRS.
double equilibrium_moisture(double temperature_c, double humidity_pct);

// Humidité des végétaux morts à chaque heure : elle tend vers l'équilibre avec l'air au ras du sol (réchauffé et
// asséché par le soleil, selon la correction du NFDRS de 1978) en 1, 10 ou 100 heures selon leur grosseur, et vers
// la saturation sous la pluie. La première heure part de l'équilibre : prévoir une dizaine de jours de relevés
// avant le feu pour que les branches de 100 h aient oublié ce départ.
std::vector<DeadMoisture> dead_fuel_moisture(std::span<const WeatherHour> hours);

class Weather {
public:
    explicit Weather(std::vector<WeatherPeriod> periods);
    static Weather constant(double wind_kmh, double wind_from_deg, const Moisture& moisture);
    // CSV : heure, vent_kmh, vent_de_deg, m1h, m10h, m100h, herbe, ligneux (humidités en %).
    static Weather from_csv(const std::filesystem::path& path);
    // Météo réelle : une période par heure à partir de `ignition_hour`, humidité des végétaux morts calculée depuis
    // la première heure des relevés ; les végétaux vivants gardent l'humidité donnée (fractions).
    static Weather from_hours(std::span<const WeatherHour> hours, size_t ignition_hour, double live_herb, double live_woody);

    size_t period_at(double t_min) const;
    const WeatherPeriod& operator[](size_t i) const { return periods_[i]; }
    size_t size() const { return periods_.size(); }

private:
    std::vector<WeatherPeriod> periods_;
};

// Scénarios d'humidité standard de Scott & Burgan : D1L1 (très sec) à D4L4 (humide),
// ou leurs noms : extreme = D1L1, tres_sec = D2L2, sec = D3L3, modere = D4L4.
std::optional<Moisture> moisture_scenario(std::string_view name);

}  // namespace firesim

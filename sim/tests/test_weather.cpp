#include <cmath>
#include <stdexcept>
#include <vector>

#include "check.hpp"
#include "firesim/weather.hpp"

using namespace firesim;

namespace {

double fahrenheit_to_c(double f) { return (f - 32.0) * 5.0 / 9.0; }

std::vector<WeatherHour> repeat(const WeatherHour& hour, size_t count) { return std::vector<WeatherHour>(count, hour); }

}  // namespace

TEST_CASE(equilibrium_moisture_follows_simard) {
    // Les trois équations de Simard (1968), en °F et en %
    CHECK_NEAR(equilibrium_moisture(fahrenheit_to_c(80), 5), (0.03229 + 0.281073 * 5 - 0.000578 * 5 * 80) / 100, 1e-9);
    CHECK_NEAR(equilibrium_moisture(fahrenheit_to_c(90), 20), (2.22749 + 0.160107 * 20 - 0.01478 * 90) / 100, 1e-9);
    CHECK_NEAR(equilibrium_moisture(fahrenheit_to_c(70), 50),
               (21.0606 + 0.005565 * 2500 - 0.00035 * 50 * 70 - 0.483199 * 50) / 100, 1e-9);
    CHECK(equilibrium_moisture(30, 20) < equilibrium_moisture(15, 20));  // plus chaud, plus sec
    CHECK(equilibrium_moisture(20, 20) < equilibrium_moisture(20, 80));
}

TEST_CASE(dead_moisture_settles_at_equilibrium) {
    const auto dead = dead_fuel_moisture(repeat({0, 0, 25, 40, 0, 0}, 300));
    const double emc = equilibrium_moisture(25, 40);
    CHECK_NEAR(dead.back().d1h, emc, 1e-9);
    CHECK_NEAR(dead.back().d10h, emc, 1e-9);
    CHECK_NEAR(dead.back().d100h, emc, 1e-9);
}

TEST_CASE(dead_moisture_lags_by_size) {
    // Après une longue période humide, une heure d'air sec : chaque classe fait 1 - e^(-1/tau) du chemin.
    auto hours = repeat({0, 0, 20, 80, 0, 0}, 200);
    hours.push_back({0, 0, 20, 20, 0, 0});
    const auto dead = dead_fuel_moisture(hours);
    const double wet = equilibrium_moisture(20, 80), dry = equilibrium_moisture(20, 20);
    CHECK_NEAR(dead.back().d1h, dry + (wet - dry) * std::exp(-1.0), 1e-9);
    CHECK_NEAR(dead.back().d10h, dry + (wet - dry) * std::exp(-0.1), 1e-9);
    CHECK_NEAR(dead.back().d100h, dry + (wet - dry) * std::exp(-0.01), 1e-9);
}

TEST_CASE(sun_dries_fine_fuels) {
    // Plein soleil : végétaux au ras du sol plus chauds de 25 °F, air plus sec d'un quart (NFDRS 1978).
    const auto shade = dead_fuel_moisture(repeat({0, 0, 30, 30, 0, 0}, 1));
    const auto sun = dead_fuel_moisture(repeat({0, 0, 30, 30, 900, 0}, 1));
    CHECK_NEAR(sun[0].d1h, equilibrium_moisture(30 + 25.0 * 5.0 / 9.0, 30 * 0.75), 1e-9);
    CHECK(sun[0].d1h < shade[0].d1h);
    // Été corse, début d'après-midi : brindilles mortes à 3-5 %, comme le scénario « extrême » (D1L1)
    CHECK(sun[0].d1h > 0.025 && sun[0].d1h < 0.05);
}

TEST_CASE(rain_wets_fine_fuels_first) {
    auto hours = repeat({0, 0, 28, 30, 0, 0}, 100);
    for (int i = 0; i < 3; ++i) hours.push_back({0, 0, 18, 95, 0, 2.0});
    const auto dead = dead_fuel_moisture(hours);
    CHECK(dead.back().d1h > 0.30);
    CHECK(dead.back().d10h > dead.front().d10h + 0.05);
    CHECK(dead.back().d100h < 0.12);
}

TEST_CASE(weather_from_hours_starts_at_ignition) {
    std::vector<WeatherHour> hours;
    for (int i = 0; i < 48; ++i) hours.push_back({10.0 + i, 200.0 + i, 20, 40, i % 24 > 8 && i % 24 < 18 ? 600.0 : 0.0, 0});
    const Weather weather = Weather::from_hours(hours, 24, 0.3, 0.6);
    const auto dead = dead_fuel_moisture(hours);
    CHECK(weather.size() == 24);
    CHECK_NEAR(weather[0].start_min, 0.0, 1e-12);
    CHECK_NEAR(weather[3].start_min, 180.0, 1e-12);
    CHECK_NEAR(weather[3].wind_kmh, 37.0, 1e-12);
    CHECK_NEAR(weather[3].wind_from_deg, 227.0, 1e-12);
    CHECK_NEAR(weather[3].moisture.d1h, dead[27].d1h, 1e-12);
    CHECK_NEAR(weather[3].moisture.live_herb, 0.3, 1e-12);
    CHECK_NEAR(weather[3].moisture.live_woody, 0.6, 1e-12);
    CHECK(weather.period_at(130.0) == 2);

    bool threw = false;
    try {
        Weather::from_hours(hours, 48, 0.3, 0.6);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE(hourly_weather_finds_ignition_time) {
    HourlyWeather weather{{"2025-07-20T12:00", "2025-07-20T13:00"}, repeat({}, 2)};
    CHECK(weather.index_of("2025-07-20T13:00") == 1);
    bool threw = false;
    try {
        weather.index_of("2025-07-20T14:00");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

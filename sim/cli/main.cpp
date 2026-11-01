// firesim : simulation de propagation d'un feu sur la carte de la Corse.
//
//   firesim run --ignition 42.55,8.80 --wind 40 --wind-from 315 --moisture extreme --duration 6h --out runs/test
//   firesim point --fuel SH5 --moisture D1L1 --wind 30 --slope 20
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "firesim/io.hpp"
#include "firesim/lambert93.hpp"
#include "firesim/spread.hpp"

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif

using namespace firesim;
namespace fs = std::filesystem;

namespace {

constexpr const char* kUsage = R"(firesim - propagation d'un feu de forêt (modèle de Rothermel)

firesim run   simule un feu sur la carte et écrit le résultat dans un dossier
  --ignition LAT,LON     départ de feu en coordonnées GPS (répétable), ex. 42.55,8.80
  --ignition-xy X,Y      départ de feu en Lambert 93 (répétable)
  --duration 6h          durée simulée (h ou min)                            [6h]
  --wind KMH             vent moyen à 10 m                                    [0]
  --wind-from DEG        direction d'où vient le vent (0 = nord, 315 = nord-ouest) [0]
  --moisture NOM         humidité des végétaux : extreme, tres_sec, sec, modere, ou D1L1 à D4L4 [extreme]
  --weather FICHIER      météo heure par heure (CSV), à la place de --wind et --moisture
  --meteo FICHIER        météo réelle heure par heure (CSV de firesim_pipeline.meteo), à la place de --wind :
                         l'humidité des végétaux morts en est calculée, --moisture ne règle que les vivants
  --start DATE_HEURE     avec --meteo : heure locale du départ du feu, ex. 2025-07-20T13:00
  --landscape DOSSIER    couches exportées par le pipeline                    [data/processed/sim]
  --breach-factor F      une coupure de largeur L résiste si L >= F x longueur de flamme [1.5]
  --no-canopy            pas de réduction du vent sous les arbres
  --canopy-cover F       part du sol couverte par les arbres, pour cette réduction [0.5]
  --no-terrain-wind      vent uniforme, sans la correction par le relief (atlas WindNinja)
  --ros-factor F         multiplie la vitesse, 1 = Rothermel seul ; calibré sur les feux réels [0.44]
  --wind-factor F        multiplie le vent                                    [1]
  --fuel-swap DE=VERS    brûle un modèle de combustible comme un autre (répétable), ex. TU5=SH5
  --out DOSSIER          dossier de sortie (obligatoire)

firesim point  comportement du feu sur un terrain homogène (Rothermel seul, sans facteur de vitesse)
  --fuel CODE            modèle de combustible (SH5, 145…)
  --moisture NOM  --wind KMH  --slope DEG
)";

struct Args {
    std::string command;
    std::multimap<std::string, std::string> values;
    std::vector<std::string> flags;

    static Args parse(int argc, char** argv) {
        Args a;
        if (argc > 1) a.command = argv[1];
        for (int i = 2; i < argc; ++i) {
            std::string key = argv[i];
            if (key.rfind("--", 0) != 0) throw std::invalid_argument("argument inattendu : " + key);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                a.values.emplace(key, argv[++i]);
            } else {
                a.flags.push_back(key);
            }
        }
        return a;
    }
    std::string get(const std::string& key, const std::string& fallback) const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    std::vector<std::string> all(const std::string& key) const {
        std::vector<std::string> out;
        for (auto [it, end] = values.equal_range(key); it != end; ++it) out.push_back(it->second);
        return out;
    }
    bool has(const std::string& key) const {
        return values.contains(key) || std::find(flags.begin(), flags.end(), key) != flags.end();
    }
};

std::pair<double, double> parse_pair(const std::string& s) {
    const auto comma = s.find(',');
    if (comma == std::string::npos) throw std::invalid_argument("deux valeurs séparées par une virgule attendues : " + s);
    return {std::stod(s.substr(0, comma)), std::stod(s.substr(comma + 1))};
}

double parse_duration_min(const std::string& s) {
    size_t pos = 0;
    const double value = std::stod(s, &pos);
    const std::string unit = s.substr(pos);
    if (unit == "h") return value * 60.0;
    if (unit.empty() || unit == "m" || unit == "min") return value;
    throw std::invalid_argument("durée invalide : " + s);
}

Moisture parse_moisture(const std::string& name) {
    const auto m = moisture_scenario(name);
    if (!m) throw std::invalid_argument("humidité inconnue : " + name);
    return *m;
}

const FuelModel* parse_fuel(const std::string& s) {
    for (int n = 0; n < 256; ++n) {
        const FuelModel* m = find_fuel_model(n);
        if (m && (m->code == s || std::to_string(n) == s)) return m;
    }
    throw std::invalid_argument("modèle de combustible inconnu : " + s);
}

Weather parse_weather(const Args& args) {
    const Moisture scenario = parse_moisture(args.get("--moisture", "extreme"));
    if (args.has("--meteo")) {
        if (!args.has("--start")) throw std::invalid_argument("--meteo demande --start (heure du départ du feu)");
        const HourlyWeather hourly = HourlyWeather::from_csv(args.get("--meteo", ""));
        return Weather::from_hours(hourly.hours, hourly.index_of(args.get("--start", "")), scenario.live_herb,
                                   scenario.live_woody);
    }
    if (args.has("--weather")) return Weather::from_csv(args.get("--weather", ""));
    return Weather::constant(std::stod(args.get("--wind", "0")), std::stod(args.get("--wind-from", "0")), scenario);
}

void write_layer(const fs::path& path, const std::vector<float>& data) { io::write_raw<float>(path, data); }

int run_command(const Args& args) {
    if (!args.has("--out")) throw std::invalid_argument("--out est obligatoire");
    const fs::path out_dir = args.get("--out", "");
    const fs::path landscape_dir = args.get("--landscape", "data/processed/sim");

    std::vector<Ignition> ignitions;
    for (const auto& s : args.all("--ignition")) {
        const auto [lat, lon] = parse_pair(s);
        const Lambert93 p = to_lambert93({lat, lon});
        ignitions.push_back({p.x, p.y});
    }
    for (const auto& s : args.all("--ignition-xy")) {
        const auto [x, y] = parse_pair(s);
        ignitions.push_back({x, y});
    }
    if (ignitions.empty()) throw std::invalid_argument("au moins un --ignition est nécessaire");

    const Weather weather = parse_weather(args);
    SpreadOptions options;
    options.duration_min = parse_duration_min(args.get("--duration", "6h"));
    options.breach_factor = std::stod(args.get("--breach-factor", std::to_string(options.breach_factor)));
    options.ros_factor = std::stod(args.get("--ros-factor", std::to_string(options.ros_factor)));
    options.wind_factor = std::stod(args.get("--wind-factor", std::to_string(options.wind_factor)));
    if (args.has("--canopy-cover") && options.canopy) options.canopy->cover = std::stod(args.get("--canopy-cover", ""));
    if (args.has("--no-canopy")) options.canopy.reset();
    if (args.has("--no-terrain-wind")) options.terrain_wind = false;
    for (const auto& s : args.all("--fuel-swap")) {
        const auto eq = s.find('=');
        if (eq == std::string::npos) throw std::invalid_argument("--fuel-swap attend DE=VERS, ex. TU5=SH5 : " + s);
        options.fuel_swap.emplace_back(static_cast<uint8_t>(parse_fuel(s.substr(0, eq))->number),
                                       static_cast<uint8_t>(parse_fuel(s.substr(eq + 1))->number));
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const Landscape land = Landscape::load(landscape_dir);
    const auto t1 = clock::now();
    Simulator sim(land);
    const SpreadResult result = sim.run(weather, ignitions, options);
    const auto t2 = clock::now();
    const bool terrain_wind = options.terrain_wind && land.wind.has_value();
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };

    // Fenêtre englobant la zone brûlée
    const GridInfo& g = land.grid;
    const int wc = std::max(0, result.col_max - result.col_min + 1);
    const int wr = std::max(0, result.row_max - result.row_min + 1);
    const size_t n = static_cast<size_t>(wc) * wr;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> arrival(n, nan), ros(n, nan), flame(n, nan), intensity(n, nan);
    double max_ros = 0, max_flame = 0, last_arrival = 0;
    for (const auto& c : result.cells) {
        const int col = static_cast<int>(c.index % g.width) - result.col_min;
        const int row = static_cast<int>(c.index / g.width) - result.row_min;
        const size_t i = static_cast<size_t>(row) * wc + col;
        arrival[i] = c.arrival_min;
        ros[i] = c.ros_m_min;
        flame[i] = c.flame_m;
        intensity[i] = c.intensity_kw_m;
        max_ros = std::max(max_ros, static_cast<double>(c.ros_m_min));
        max_flame = std::max(max_flame, static_cast<double>(c.flame_m));
        last_arrival = std::max(last_arrival, static_cast<double>(c.arrival_min));
    }
    fs::create_directories(out_dir);
    write_layer(out_dir / "arrival_min.f32", arrival);
    write_layer(out_dir / "ros_m_min.f32", ros);
    write_layer(out_dir / "flame_m.f32", flame);
    write_layer(out_dir / "intensity_kw_m.f32", intensity);

    io::JsonWriter json;
    json.begin_object().field("format_version", 1);
    json.key("grid").begin_object().field("crs", "EPSG:2154").field("left", g.left).field("top", g.top).field("res", g.res)
        .field("width", g.width).field("height", g.height).end_object();
    json.key("window").begin_object().field("col", result.col_min).field("row", result.row_min).field("width", wc)
        .field("height", wr).end_object();
    json.key("layers").begin_object().field("arrival_min", "arrival_min.f32").field("ros_m_min", "ros_m_min.f32")
        .field("flame_m", "flame_m.f32").field("intensity_kw_m", "intensity_kw_m.f32").end_object();
    json.key("ignitions").begin_array();
    for (const auto& ig : ignitions) {
        const LatLon ll = from_lambert93({ig.x, ig.y});
        json.begin_object().field("x", ig.x).field("y", ig.y).field("lat", ll.lat).field("lon", ll.lon)
            .field("time_min", ig.time_min).end_object();
    }
    json.end_array();
    json.key("weather").begin_array();
    for (size_t i = 0; i < weather.size(); ++i) {
        const auto& p = weather[i];
        json.begin_object().field("start_min", p.start_min).field("wind_kmh", p.wind_kmh).field("wind_from_deg", p.wind_from_deg)
            .field("m1h", p.moisture.d1h).field("m10h", p.moisture.d10h).field("m100h", p.moisture.d100h)
            .field("live_herb", p.moisture.live_herb).field("live_woody", p.moisture.live_woody).end_object();
    }
    json.end_array();
    json.key("options").begin_object().field("duration_min", options.duration_min).field("breach_factor", options.breach_factor)
        .field("canopy_sheltering", options.canopy.has_value()).field("canopy_cover", options.canopy ? options.canopy->cover : 0.0)
        .field("terrain_wind", terrain_wind).field("ros_factor", options.ros_factor).field("wind_factor", options.wind_factor);
    json.key("fuel_swap").begin_array();
    for (const auto [from, to] : options.fuel_swap) {
        json.begin_object().field("from", find_fuel_model(from)->code).field("to", find_fuel_model(to)->code).end_object();
    }
    json.end_array().end_object();
    json.key("summary").begin_object().field("burned_cells", static_cast<int64_t>(result.cells.size()))
        .field("area_ha", result.area_ha).field("max_ros_m_min", max_ros).field("max_flame_m", max_flame)
        .field("last_arrival_min", last_arrival).field("reached_grid_edge", result.reached_grid_edge)
        .field("compute_ms", ms(t2 - t1)).end_object();
    json.end_object();
    json.save(out_dir / "run.json");

    const WeatherPeriod& start = weather[0];
    std::printf("Au départ : vent %.0f km/h venant de %.0f°, végétaux morts à %.1f / %.1f / %.1f %% (1 h, 10 h, 100 h)\n",
                start.wind_kmh, start.wind_from_deg, start.moisture.d1h * 100, start.moisture.d10h * 100,
                start.moisture.d100h * 100);
    std::printf("Surface brûlée : %.0f ha en %.1f h\n", result.area_ha, options.duration_min / 60.0);
    std::printf("Vitesse maximale : %.1f m/min (%.2f km/h), flammes jusqu'à %.1f m\n", max_ros, max_ros * 0.06, max_flame);
    std::printf("Vent : %s\n", terrain_wind ? "corrigé par le relief (atlas WindNinja)"
                                            : land.wind ? "uniforme (atlas ignoré)" : "uniforme (pas d'atlas des vents)");
    if (result.reached_grid_edge) std::printf("Attention : le feu a atteint le bord de la carte.\n");
    std::printf("Calcul : %.0f ms (chargement de la carte : %.0f ms)\n", ms(t2 - t1), ms(t1 - t0));
    std::printf("Résultat : %s\n", out_dir.string().c_str());
    return 0;
}

int point_command(const Args& args) {
    const FuelModel& model = *parse_fuel(args.get("--fuel", "SH5"));
    const Moisture moisture = parse_moisture(args.get("--moisture", "extreme"));
    const double wind = std::stod(args.get("--wind", "0"));
    const double slope = std::stod(args.get("--slope", "0"));
    const FuelBed bed = fuel_bed(model, moisture);
    const double waf = wind_adjustment_factor(model.depth_ft);
    const double midflame = wind * kFtMinPerKmh * k10mTo20ft * waf;
    const SpreadVector v = spread_vector(bed, midflame, 0.0, slope, 0.0);  // vent dans le sens de la pente
    const double intensity = fireline_intensity(bed, v.ros);
    std::printf("%s (%d), humidité 1 h %.0f %%, vent %.0f km/h à 10 m (%.1f km/h à mi-flamme), pente %.0f°\n",
                std::string(model.code).c_str(), model.number, moisture.d1h * 100, wind, midflame / kFtMinPerKmh, slope);
    std::printf("  vitesse         %.1f m/min (%.2f km/h), sans vent ni pente %.2f m/min\n", v.ros * kMetersPerFoot,
                v.ros * kMetersPerFoot * 0.06, bed.r0 * kMetersPerFoot);
    std::printf("  flammes         %.1f m\n", flame_length_ft(intensity) * kMetersPerFoot);
    std::printf("  intensité       %.0f kW/m\n", intensity * kKwPerMPerBtuFtS);
    std::printf("  forme           %.1f fois plus long que large\n", v.length_to_width);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);  // UTF-8
#endif
    try {
        const Args args = Args::parse(argc, argv);
        if (args.command == "run") return run_command(args);
        if (args.command == "point") return point_command(args);
        std::fputs(kUsage, stdout);
        return args.command.empty() || args.command == "help" ? 0 : 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Erreur : %s\n", e.what());
        return 1;
    }
}

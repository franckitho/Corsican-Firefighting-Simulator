// Compare le modèle de Rothermel à BehavePlus (behave) sur ~4 900 cas : 40 modèles de combustible,
// 5 scénarios d'humidité, vents, pentes, couvert forestier. Références : tests/reference/behave_surface.csv,
// produites par tools/behave_reference.
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <sstream>

#include "check.hpp"
#include "firesim/rothermel.hpp"

using namespace firesim;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kTolerance = 1e-6;  // même calcul, même ordre d'opérations : l'écart doit être nul

std::vector<std::map<std::string, double>> load_csv(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::vector<std::map<std::string, double>> rows;
    std::string line;
    std::vector<std::string> header;
    if (!std::getline(in, line)) return rows;
    std::stringstream hs(line);
    for (std::string h; std::getline(hs, h, ',');) header.push_back(h);
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::map<std::string, double> row;
        size_t i = 0;
        for (std::string cell; std::getline(ss, cell, ',') && i < header.size(); ++i) row[header[i]] = std::stod(cell);
        rows.push_back(std::move(row));
    }
    return rows;
}

double angle_diff_deg(double a, double b) {
    const double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

}  // namespace

TEST_CASE(rothermel_matches_behaveplus) {
    auto rows = load_csv(std::filesystem::path(FIRESIM_TEST_DATA) / "behave_surface.csv");
    CHECK(rows.size() > 4000);

    int reported = 0;
    auto expect = [&](bool ok, std::map<std::string, double>& r, const char* what, double got, double want) {
        if (ok) return;
        if (++reported <= 15) {
            firesim::test::fail(__FILE__, __LINE__,
                                std::string(what) + " : " + std::to_string(got) + " au lieu de " + std::to_string(want) +
                                    " (modèle " + std::to_string(static_cast<int>(r["fuel"])) + ", vent " +
                                    std::to_string(r["wind_mph"]) + " mi/h, pente " + std::to_string(r["slope_pct"]) +
                                    " %, humidité 1 h " + std::to_string(r["m1"]) + " %, herbe " +
                                    std::to_string(r["mherb"]) + " %)");
        } else {
            ++firesim::test::failures();
        }
    };

    for (auto& r : rows) {
        const FuelModel* model = find_fuel_model(static_cast<int>(r["fuel"]));
        CHECK(model != nullptr);
        if (model == nullptr) continue;
        const Moisture moisture{r["m1"] / 100, r["m10"] / 100, r["m100"] / 100, r["mherb"] / 100, r["mwoody"] / 100};
        const FuelBed bed = fuel_bed(*model, moisture);

        double midflame = r["wind_mph"] * kFtMinPerMph;
        if (r["wind_mode"] == 2) {  // vent à 10 m
            const Canopy canopy{r["canopy_cover"], r["canopy_height_ft"], r["crown_ratio"]};
            midflame = r["wind_mph"] * kFtMinPerMph * k10mTo20ft * wind_adjustment_factor(model->depth_ft, &canopy);
        }
        const double slope_deg = std::atan(r["slope_pct"] / 100.0) / kDeg;
        const double upslope = std::fmod(r["aspect_deg"] + 180.0, 360.0) * kDeg;
        const double wind_toward = std::fmod(r["wind_from_deg"] + 180.0, 360.0) * kDeg;
        const SpreadVector v = spread_vector(bed, midflame, wind_toward, slope_deg, upslope);
        const double intensity = fireline_intensity(bed, v.ros);

        using firesim::test::near;
        expect(near(midflame / kFtMinPerMph, r["midflame_mph"], kTolerance), r, "vent à mi-flamme", midflame / kFtMinPerMph, r["midflame_mph"]);
        expect(near(bed.reaction_intensity, r["reaction_intensity"], kTolerance), r, "intensité de réaction", bed.reaction_intensity, r["reaction_intensity"]);
        expect(near(v.ros, r["ros_ftmin"], kTolerance), r, "vitesse", v.ros, r["ros_ftmin"]);
        expect(near(v.length_to_width, r["length_to_width"], kTolerance), r, "rapport longueur/largeur", v.length_to_width, r["length_to_width"]);
        expect(near(v.eccentricity, r["eccentricity"], kTolerance), r, "excentricité", v.eccentricity, r["eccentricity"]);
        expect(near(intensity, r["intensity_btu_ft_s"], kTolerance), r, "intensité du front", intensity, r["intensity_btu_ft_s"]);
        expect(near(flame_length_ft(intensity), r["flame_ft"], kTolerance), r, "longueur de flamme", flame_length_ft(intensity), r["flame_ft"]);
        const double backing = v.ros_toward(v.direction + std::numbers::pi);
        expect(near(backing, r["backing_ftmin"], kTolerance), r, "vitesse arrière", backing, r["backing_ftmin"]);
        // Direction : behave arrondit à la montée les écarts de moins de 0,5°.
        if (v.ros > bed.r0 * (1 + 1e-9)) {
            const double dir = v.direction / kDeg;
            expect(angle_diff_deg(dir, r["direction_deg"]) < 0.5 + 1e-6, r, "direction", dir, r["direction_deg"]);
        }
    }
}

TEST_CASE(flame_length_of_known_intensity) {
    // Byram : 0,45 x I^0,46 (ft, BTU/ft/s) ; 1 000 BTU/ft/s -> 10,6 ft
    CHECK_NEAR(flame_length_ft(1000.0), 0.45 * std::pow(1000.0, 0.46), 1e-12);
    CHECK(flame_length_ft(0.0) == 0.0);
}

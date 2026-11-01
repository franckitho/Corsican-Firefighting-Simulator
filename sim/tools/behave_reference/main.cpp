// Génère les valeurs de référence des tests à partir de behave, le moteur de calcul de BehavePlus
// (USFS Missoula Fire Lab, domaine public : https://github.com/firelab/behave).
//
//   behave_reference > ../../tests/reference/behave_surface.csv
#include <cstdio>
#include <vector>

#include "fuelModels.h"
#include "surface.h"

struct MoistureCase {
    const char* name;
    double d1, d10, d100, herb, woody;  // %
};

struct CanopyCase {
    double cover, height_ft, crown_ratio;
};

int main() {
    const int fuels[] = {101, 102, 103, 104, 105, 106, 107, 108, 109, 121, 122, 123, 124, 141,
                         142, 143, 144, 145, 146, 147, 148, 149, 161, 162, 163, 164, 165, 181,
                         182, 183, 184, 185, 186, 187, 188, 189, 201, 202, 203, 204};
    const MoistureCase moistures[] = {
        {"D1L1", 3, 4, 5, 30, 60},    {"D2L2", 6, 7, 8, 60, 90},     {"D3L3", 9, 10, 11, 90, 120},
        {"D4L4", 12, 13, 14, 120, 150}, {"D1L3", 3, 4, 5, 90, 120},
    };
    const double midflame_winds_mph[] = {0, 4, 12, 25};
    const double wind_from_deg[] = {0, 250};
    const double slopes_pct[] = {0, 25, 70};
    const double aspect_deg = 200;
    const CanopyCase canopies[] = {{0, 0, 0}, {0.5, 49.2, 0.4}, {0.1, 49.2, 0.4}};

    FuelModels fuelModels;
    Surface surface(fuelModels);

    std::printf("fuel,m1,m10,m100,mherb,mwoody,wind_mode,wind_mph,wind_from_deg,slope_pct,aspect_deg,"
                "canopy_cover,canopy_height_ft,crown_ratio,midflame_mph,ros_ftmin,direction_deg,"
                "length_to_width,eccentricity,reaction_intensity,intensity_btu_ft_s,flame_ft,backing_ftmin,"
                "flanking_ftmin\n");

    auto run = [&](int fuel, const MoistureCase& m, WindHeightInputMode::WindHeightInputModeEnum mode, double wind,
                   double from, double slope, const CanopyCase& c) {
        surface.updateSurfaceInputs(fuel, m.d1, m.d10, m.d100, m.herb, m.woody, FractionUnits::Percent, wind,
                                    SpeedUnits::MilesPerHour, mode, from, WindAndSpreadOrientationMode::RelativeToNorth,
                                    slope, SlopeUnits::Percent, aspect_deg, c.cover, FractionUnits::Fraction,
                                    c.height_ft, LengthUnits::Feet, c.crown_ratio, FractionUnits::Fraction);
        surface.setWindAdjustmentFactorCalculationMethod(WindAdjustmentFactorCalculationMethod::UseCrownRatio);
        surface.doSurfaceRunInDirectionOfMaxSpread();
        std::printf("%d,%g,%g,%g,%g,%g,%d,%g,%g,%g,%g,%g,%g,%g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g\n",
                    fuel, m.d1, m.d10, m.d100, m.herb, m.woody, static_cast<int>(mode), wind, from, slope, aspect_deg,
                    c.cover, c.height_ft, c.crown_ratio, surface.getMidflameWindspeed(SpeedUnits::MilesPerHour),
                    surface.getSpreadRate(SpeedUnits::FeetPerMinute), surface.getDirectionOfMaxSpread(),
                    surface.getFireLengthToWidthRatio(), surface.getFireEccentricity(),
                    surface.getReactionIntensity(HeatSourceAndReactionIntensityUnits::BtusPerSquareFootPerMinute),
                    surface.getFirelineIntensity(FirelineIntensityUnits::BtusPerFootPerSecond),
                    surface.getFlameLength(LengthUnits::Feet), surface.getBackingSpreadRate(SpeedUnits::FeetPerMinute),
                    surface.getFlankingSpreadRate(SpeedUnits::FeetPerMinute));
    };

    for (int fuel : fuels) {
        for (const auto& m : moistures)
            for (double wind : midflame_winds_mph)
                for (double from : wind_from_deg)
                    for (double slope : slopes_pct)
                        run(fuel, m, WindHeightInputMode::DirectMidflame, wind, from, slope, canopies[0]);
        // Vent à 10 m réduit par le couvert (sans couvert, abrité, couvert trop clairsemé)
        for (const auto& c : canopies) run(fuel, moistures[0], WindHeightInputMode::TenMeter, 20, 250, 25, c);
    }
    return 0;
}

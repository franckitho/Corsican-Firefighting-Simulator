#pragma once

#include "firesim/fuel_models.hpp"

// Modèle de propagation d'un feu de surface de Rothermel (1972), avec les compléments
// d'Albini (1976) et les choix de BehavePlus (behave), dont il reproduit les résultats.
// Unités impériales en interne : ft, lb, BTU, minutes ; conversions en bas de fichier.
namespace firesim {

// Humidités des combustibles (fractions : 0.06 = 6 %).
struct Moisture {
    double d1h, d10h, d100h;      // morts : brindilles, branches, grosses branches
    double live_herb, live_woody;  // vivants
};

// Couvert forestier, pour réduire le vent au niveau des flammes.
struct Canopy {
    double cover;        // fraction du sol couverte
    double height_ft;
    double crown_ratio;  // part de la hauteur des arbres occupée par le houppier
};

// Ce qui ne dépend que du combustible et de son humidité (ni du vent, ni de la pente).
struct FuelBed {
    double r0 = 0;                  // vitesse sans vent ni pente (ft/min)
    double reaction_intensity = 0;  // BTU/ft²/min
    double residence_time = 0;      // min
    double packing_ratio = 0;
    double relative_packing = 0;
    double wind_b = 0, wind_c = 0, wind_e = 0;
    double slope_k = 0;             // facteur de pente = slope_k * tan²(pente)
    double depth_ft = 0;

    bool burns() const { return r0 > 0; }
};

FuelBed fuel_bed(const FuelModel& model, const Moisture& moisture);

// Comportement du feu en un point : ellipse de propagation depuis le point d'allumage.
struct SpreadVector {
    double ros = 0;             // vitesse maximale, vers l'avant (ft/min)
    double direction = 0;       // azimut de propagation maximale (radians, depuis le nord, sens horaire)
    double eccentricity = 0;
    double length_to_width = 1;
    double effective_wind = 0;  // ft/min

    // Vitesse dans la direction `azimuth` (radians), Catchpole et al. (1982).
    double ros_toward(double azimuth) const;
};

// midflame_wind : vent à mi-hauteur des flammes (ft/min) ; wind_toward : azimut vers lequel
// souffle le vent ; slope_deg : pente ; upslope : azimut de la montée (radians).
SpreadVector spread_vector(const FuelBed& bed, double midflame_wind, double wind_toward, double slope_deg,
                           double upslope, bool wind_limit = false);

double fireline_intensity(const FuelBed& bed, double ros_ftmin);  // BTU/ft/s (Byram)
double flame_length_ft(double intensity_btu_ft_s);                // Byram (1959)

// Rapport vent à mi-flamme / vent à 20 ft. Sans couvert (ou couvert trop clairsemé) :
// profil logarithmique au-dessus de la strate combustible.
double wind_adjustment_factor(double depth_ft, const Canopy* canopy = nullptr);

// Conversions
constexpr double kMetersPerFoot = 0.3048;
constexpr double kFtMinPerKmh = 1000.0 / kMetersPerFoot / 60.0;
constexpr double kFtMinPerMph = 88.0;
constexpr double kKwPerMPerBtuFtS = 3.46414;
constexpr double k10mTo20ft = 1.0 / 1.15;  // vent à 10 m -> vent à 20 ft (convention BehavePlus)

}  // namespace firesim

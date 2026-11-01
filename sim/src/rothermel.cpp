#include "firesim/rothermel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace firesim {
namespace {

constexpr double kTpaToLbFt2 = 2000.0 / 43560.0;
constexpr double kParticleDensity = 32.0;  // lb/ft³ (Albini 1976)
constexpr double kTotalSilica = 0.0555;
constexpr double kEffectiveSilica = 0.01;

struct Particle {
    double load = 0;  // lb/ft²
    double sav = 0;   // 1/ft
    double moisture = 0;
    double heat = 0;
};

// Classes de taille d'Albini (1976) servant à pondérer la charge nette.
int size_class(double sav) {
    if (sav >= 1200.0) return 0;
    if (sav >= 192.0) return 1;
    if (sav >= 96.0) return 2;
    if (sav >= 48.0) return 3;
    if (sav >= 16.0) return 4;
    return 5;  // ignorée
}

// Grandeurs moyennes d'un état (morts ou vivants), pondérées par la surface des particules.
struct LifeState {
    double area = 0;
    double net_load = 0;
    double heat = 0, silica = 0, moisture = 0, sav = 0;
    double heat_sink = 0;  // somme f_i * Qig_i * exp(-138 / sav_i), à multiplier par la densité apparente
};

template <size_t N>
LifeState summarize(const std::array<Particle, N>& particles) {
    LifeState s;
    std::array<double, N> area{};
    for (size_t i = 0; i < N; ++i) {
        area[i] = particles[i].load * particles[i].sav / kParticleDensity;
        s.area += area[i];
    }
    if (s.area <= 1e-7) return s;

    std::array<double, N> fraction{};
    std::array<double, 6> class_weight{};
    for (size_t i = 0; i < N; ++i) {
        fraction[i] = area[i] / s.area;
        class_weight[size_class(particles[i].sav)] += fraction[i];
    }
    class_weight[5] = 0.0;

    for (size_t i = 0; i < N; ++i) {
        const Particle& p = particles[i];
        if (p.sav <= 1e-7) continue;
        s.net_load += class_weight[size_class(p.sav)] * p.load * (1.0 - kTotalSilica);
        s.heat += fraction[i] * p.heat;
        s.silica += fraction[i] * kEffectiveSilica;
        s.moisture += fraction[i] * p.moisture;
        s.sav += fraction[i] * p.sav;
        s.heat_sink += fraction[i] * (250.0 + 1116.0 * p.moisture) * std::exp(-138.0 / p.sav);
    }
    return s;
}

double moisture_damping(double moisture, double extinction) {
    if (extinction <= 0.0 || moisture >= extinction) return 0.0;
    const double r = moisture / extinction;
    return 1.0 - 2.59 * r + 5.11 * r * r - 3.52 * r * r * r;
}

double mineral_damping(double effective_silica) {
    const double d = std::pow(effective_silica, 0.19);
    return d < 1e-6 ? 0.0 : std::min(0.174 / d, 1.0);
}

constexpr double kTwoPi = 2.0 * std::numbers::pi;

}  // namespace

FuelBed fuel_bed(const FuelModel& model, const Moisture& m) {
    std::array<Particle, 4> dead{{
        {model.load_tpa[0] * kTpaToLbFt2, model.sav_1h, m.d1h, model.heat_dead},
        {model.load_tpa[1] * kTpaToLbFt2, 109.0, m.d10h, model.heat_dead},
        {model.load_tpa[2] * kTpaToLbFt2, 30.0, m.d100h, model.heat_dead},
        {0.0, model.sav_herb, m.d1h, model.heat_dead},  // herbe sèche (modèles dynamiques)
    }};
    std::array<Particle, 2> live{{
        {model.load_tpa[3] * kTpaToLbFt2, model.sav_herb, m.live_herb, model.heat_live},
        {model.load_tpa[4] * kTpaToLbFt2, model.sav_woody, m.live_woody, model.heat_live},
    }};
    const bool has_live = live[0].load > 0.0 || live[1].load > 0.0;

    if (model.dynamic) {
        // Une partie de l'herbe vivante est sèche : elle brûle comme un combustible mort.
        const double cured = m.live_herb < 0.30 ? 1.0 : m.live_herb <= 1.20 ? 1.333 - 1.11 * m.live_herb : 0.0;
        dead[3].load = live[0].load * cured;
        live[0].load -= dead[3].load;
    }

    FuelBed bed;
    bed.depth_ft = model.depth_ft;
    const LifeState d = summarize(dead);
    const LifeState l = summarize(live);
    const double total_area = d.area + l.area;
    if (total_area <= 1e-7 || model.depth_ft <= 0.0) return bed;
    const double f_dead = d.area / total_area;
    const double f_live = 1.0 - f_dead;

    // Humidité d'extinction des vivants : ils brûlent tant que les morts fins les sèchent assez (Albini 1976).
    double mx_live = 0.0;
    if (has_live) {
        double fine_dead = 0.0, fine_dead_moisture = 0.0, fine_live = 0.0;
        for (const auto& p : dead) {
            if (p.sav <= 1e-7) continue;
            const double w = p.load * std::exp(-138.0 / p.sav);
            fine_dead += w;
            fine_dead_moisture += w * p.moisture;
        }
        for (const auto& p : live) {
            if (p.sav > 1e-7) fine_live += p.load * std::exp(-500.0 / p.sav);
        }
        const double mf_dead = fine_dead > 1e-7 ? fine_dead_moisture / fine_dead : 0.0;
        const double ratio = fine_live > 1e-7 ? fine_dead / fine_live : 0.0;
        mx_live = std::max(2.9 * ratio * (1.0 - mf_dead / model.mx_dead) - 0.226, model.mx_dead);
    }

    const double sigma = f_dead * d.sav + f_live * l.sav;
    double total_load = 0.0;
    for (const auto& p : dead) total_load += p.load;
    for (const auto& p : live) total_load += p.load;
    const double bulk_density = total_load / model.depth_ft;
    const double beta = bulk_density / kParticleDensity;
    const double beta_rel = beta / (3.348 / std::pow(sigma, 0.8189));

    const double a = 133.0 / std::pow(sigma, 0.7913);
    const double sigma15 = std::pow(sigma, 1.5);
    const double gamma_max = sigma15 / (495.0 + 0.0594 * sigma15);
    const double gamma = gamma_max * std::pow(beta_rel, a) * std::exp(a * (1.0 - beta_rel));

    const double reaction_intensity =
        gamma * (d.net_load * d.heat * moisture_damping(d.moisture, model.mx_dead) * mineral_damping(d.silica) +
                 l.net_load * l.heat * moisture_damping(l.moisture, mx_live) * mineral_damping(l.silica));
    const double propagating_flux =
        std::exp((0.792 + 0.681 * std::sqrt(sigma)) * (beta + 0.1)) / (192.0 + 0.2595 * sigma);
    const double heat_sink = bulk_density * (f_dead * d.heat_sink + f_live * l.heat_sink);

    bed.reaction_intensity = reaction_intensity;
    bed.r0 = heat_sink < 1e-7 ? 0.0 : reaction_intensity * propagating_flux / heat_sink;
    bed.residence_time = 384.0 / sigma;
    bed.packing_ratio = beta;
    bed.relative_packing = beta_rel;
    bed.wind_c = 7.47 * std::exp(-0.133 * std::pow(sigma, 0.55));
    bed.wind_b = 0.02526 * std::pow(sigma, 0.54);
    bed.wind_e = 0.715 * std::exp(-0.000359 * sigma);
    bed.slope_k = 5.275 * std::pow(beta, -0.3);
    return bed;
}

SpreadVector spread_vector(const FuelBed& bed, double midflame_wind, double wind_toward, double slope_deg,
                           double upslope, bool wind_limit) {
    SpreadVector v;
    if (!bed.burns()) return v;

    const double phi_w = midflame_wind < 1e-7 ? 0.0
                                              : bed.wind_c * std::pow(midflame_wind, bed.wind_b) *
                                                    std::pow(bed.relative_packing, -bed.wind_e);
    const double tan_slope = std::tan(slope_deg * std::numbers::pi / 180.0);
    // behave borne le facteur de pente par la « limite de vent » (0,9 x intensité de réaction).
    const double phi_s = std::min(bed.slope_k * tan_slope * tan_slope, 0.9 * bed.reaction_intensity);

    // Le vent et la pente s'additionnent comme des vecteurs (composantes est, nord).
    const double slope_rate = bed.r0 * phi_s;
    const double wind_rate = bed.r0 * phi_w;
    const double east = slope_rate * std::sin(upslope) + wind_rate * std::sin(wind_toward);
    const double north = slope_rate * std::cos(upslope) + wind_rate * std::cos(wind_toward);
    const double magnitude = std::hypot(east, north);
    v.ros = bed.r0 + magnitude;
    v.direction = magnitude > 0.0 ? std::atan2(east, north) : upslope;
    v.direction = std::fmod(v.direction + kTwoPi, kTwoPi);

    // Vent « effectif » qui produirait seul le même effet que vent + pente.
    double phi_e = v.ros / bed.r0 - 1.0;
    v.effective_wind =
        phi_e > 0.0 ? std::pow(phi_e * std::pow(bed.relative_packing, bed.wind_e) / bed.wind_c, 1.0 / bed.wind_b) : 0.0;
    if (wind_limit && v.effective_wind > 0.9 * bed.reaction_intensity) {
        v.effective_wind = 0.9 * bed.reaction_intensity;
        phi_e = bed.wind_c * std::pow(v.effective_wind, bed.wind_b) * std::pow(bed.relative_packing, -bed.wind_e);
        v.ros = bed.r0 * (1.0 + phi_e);
    }

    // Forme de l'ellipse (Anderson 1983, coefficients de BehavePlus en mi/h)
    const double u = v.effective_wind / kFtMinPerMph;
    if (u > 1e-7) {
        v.length_to_width = std::min(0.936 * std::exp(0.1147 * u) + 0.461 * std::exp(-0.0692 * u) - 0.397, 8.0);
    }
    const double x = v.length_to_width * v.length_to_width - 1.0;
    v.eccentricity = x > 0.0 ? std::sqrt(x) / v.length_to_width : 0.0;
    return v;
}

double SpreadVector::ros_toward(double azimuth) const {
    if (ros <= 0.0) return 0.0;
    return ros * (1.0 - eccentricity) / (1.0 - eccentricity * std::cos(azimuth - direction));
}

double fireline_intensity(const FuelBed& bed, double ros_ftmin) {
    return ros_ftmin * bed.reaction_intensity * bed.residence_time / 60.0;
}

double flame_length_ft(double intensity) { return intensity < 1e-7 ? 0.0 : 0.45 * std::pow(intensity, 0.46); }

double wind_adjustment_factor(double depth_ft, const Canopy* canopy) {
    if (canopy != nullptr && canopy->cover > 1e-7) {
        const double crown_fraction = canopy->crown_ratio * canopy->cover / 3.0;
        const double h = canopy->height_ft;
        if (crown_fraction >= 0.05 && h >= 6.0) {
            return 0.555 / (std::sqrt(crown_fraction * h) * std::log((20.0 + 0.36 * h) / (0.13 * h)));
        }
    }
    if (depth_ft <= 1e-7) return 0.0;
    return 1.83 / std::log((20.0 + 0.36 * depth_ft) / (0.13 * depth_ft));
}

}  // namespace firesim

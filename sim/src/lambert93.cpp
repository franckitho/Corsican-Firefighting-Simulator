#include "firesim/lambert93.hpp"

#include <cmath>
#include <numbers>

namespace firesim {
namespace {

// Formules de la note EPSG 7-2 (Lambert Conic Conformal 2SP).
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;
constexpr double kA = 6378137.0;
constexpr double kF = 1.0 / 298.257222101;
const double kE = std::sqrt(2 * kF - kF * kF);
constexpr double kLat1 = 49.0 * kDeg, kLat2 = 44.0 * kDeg, kLat0 = 46.5 * kDeg, kLon0 = 3.0 * kDeg;
constexpr double kE0 = 700000.0, kN0 = 6600000.0;

double m(double lat) { return std::cos(lat) / std::sqrt(1 - kE * kE * std::sin(lat) * std::sin(lat)); }

double t(double lat) {
    const double es = kE * std::sin(lat);
    return std::tan(kPi / 4 - lat / 2) / std::pow((1 - es) / (1 + es), kE / 2);
}

struct Constants {
    double n, f, r0;
};

const Constants& constants() {
    static const Constants c = [] {
        const double n = (std::log(m(kLat1)) - std::log(m(kLat2))) / (std::log(t(kLat1)) - std::log(t(kLat2)));
        const double f = m(kLat1) / (n * std::pow(t(kLat1), n));
        return Constants{n, f, kA * f * std::pow(t(kLat0), n)};
    }();
    return c;
}

}  // namespace

Lambert93 to_lambert93(const LatLon& p) {
    const auto& c = constants();
    const double r = kA * c.f * std::pow(t(p.lat * kDeg), c.n);
    const double theta = c.n * (p.lon * kDeg - kLon0);
    return {kE0 + r * std::sin(theta), kN0 + c.r0 - r * std::cos(theta)};
}

LatLon from_lambert93(const Lambert93& p) {
    const auto& c = constants();
    const double dx = p.x - kE0;
    const double dy = c.r0 - (p.y - kN0);
    const double r = std::hypot(dx, dy);
    const double theta = std::atan2(dx, dy);
    const double tp = std::pow(r / (kA * c.f), 1.0 / c.n);
    double lat = kPi / 2 - 2 * std::atan(tp);
    for (int i = 0; i < 10; ++i) {
        const double es = kE * std::sin(lat);
        lat = kPi / 2 - 2 * std::atan(tp * std::pow((1 - es) / (1 + es), kE / 2));
    }
    return {lat / kDeg, (theta / c.n + kLon0) / kDeg};
}

double meridian_convergence(const Lambert93& p) {
    // Sur une projection conique, c'est l'angle polaire du point autour du sommet du cône : n * (lon - lon0).
    return std::atan2(p.x - kE0, constants().r0 - (p.y - kN0));
}

}  // namespace firesim

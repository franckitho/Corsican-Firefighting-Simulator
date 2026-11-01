#include "check.hpp"
#include "firesim/lambert93.hpp"

using namespace firesim;

namespace {

struct Point {
    const char* name;
    double lat, lon, x, y;  // valeurs de référence calculées avec pyproj (PROJ)
};

constexpr Point kPoints[] = {
    {"Ajaccio", 41.9192, 8.7386, 1176667.561, 6108273.283},
    {"Monte Cinto", 42.3797, 8.945, 1189925.324, 6160643.310},
    {"Bastia", 42.6973, 9.4509, 1228644.138, 6199127.370},
    {"Paris", 48.8566, 2.3522, 652469.023, 6862035.259},
};

}  // namespace

TEST_CASE(lambert93_matches_proj) {
    for (const auto& p : kPoints) {
        const Lambert93 xy = to_lambert93({p.lat, p.lon});
        CHECK(std::fabs(xy.x - p.x) < 0.01);
        CHECK(std::fabs(xy.y - p.y) < 0.01);
        const LatLon back = from_lambert93({p.x, p.y});
        CHECK(std::fabs(back.lat - p.lat) < 1e-7);
        CHECK(std::fabs(back.lon - p.lon) < 1e-7);
    }
}

TEST_CASE(meridian_convergence_matches_proj) {
    // pyproj : Proj(2154).get_factors(lon, lat).meridian_convergence ; nulle sur le méridien central (3° E)
    CHECK(std::fabs(meridian_convergence(to_lambert93({45.0, 3.0}))) < 1e-9);
    const double deg = 180.0 / 3.14159265358979323846;
    CHECK(std::fabs(meridian_convergence(to_lambert93({42.3797, 8.945})) * deg - 4.3137) < 1e-3);  // Monte Cinto
}

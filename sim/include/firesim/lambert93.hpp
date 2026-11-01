#pragma once

// Conversion GPS (WGS84, degrés) <-> Lambert 93 (EPSG:2154), projection conique conforme
// sur l'ellipsoïde GRS80. L'écart WGS84 / RGF93 (moins d'un mètre) est négligé.
namespace firesim {

struct Lambert93 {
    double x, y;
};

struct LatLon {
    double lat, lon;
};

Lambert93 to_lambert93(const LatLon& p);
LatLon from_lambert93(const Lambert93& p);

// Convergence des méridiens (radians) : le nord du quadrillage Lambert 93 est tourné de cet angle vers l'est
// par rapport au nord géographique (~4,5° en Corse). Azimut sur la grille = azimut géographique - convergence.
double meridian_convergence(const Lambert93& p);

}  // namespace firesim

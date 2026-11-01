#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "firesim/lambert93.hpp"
#include "firesim/landscape.hpp"
#include "firesim/spread.hpp"

// Lutte contre le feu : ce qui se passe avant que le feu n'arrive. Les chantiers de pare-feu avancent à la vitesse
// des engins, les avions partent de leurs bases réelles et font des rotations ; on en déduit, cellule par cellule,
// à quelle heure la lutte retient le feu (Treatment, utilisé par Simulator::run).
//
// Sources des chiffres, et valeurs qui restent des hypothèses : voir le README.
namespace firesim {

// --- Pare-feu ---

struct FirebreakOrder {
    std::vector<Lambert93> path;  // au moins deux points, dans le sens du chantier
    double order_min = 0;         // heure de l'ordre, en minutes depuis le départ du feu
    double width_m = 8;
    int engines = 1;
};

struct Firebreak {
    FirebreakOrder order;
    std::vector<uint32_t> cells;   // dans l'ordre du chantier
    std::vector<float> built_min;  // heure d'achèvement de chaque cellule
    double length_m = 0;
    double start_min = 0;   // arrivée des engins sur le premier point
    double finish_min = 0;  // ligne entièrement opérationnelle
    double holds_flame_m = 0;
};

// Lève std::invalid_argument si le tracé sort de la carte.
Firebreak plan_firebreak(const Landscape& land, const FirebreakOrder& order, double breach_factor);

// --- Avions ---

enum class AircraftKind : uint8_t { Canadair, Dash };

struct AircraftType {
    const char* name;    // « Canadair »
    const char* radio;   // indicatif : « Pélican »
    const char* fluid;   // « eau »
    double cruise_kmh;
    double drop_kmh;
    double drop_height_m;
    double strip_m;             // longueur de la bande traitée par un largage
    double lasting_min;         // durée de vie du produit au sol
    double hold_min;            // temps pendant lequel il retient le feu, une fois au contact
    double max_intensity_kw_m;  // au-delà, le feu passe
};
const AircraftType& aircraft_type(AircraftKind kind);

struct DropOrder {
    AircraftKind kind = AircraftKind::Canadair;
    Lambert93 from{}, to{};  // axe à traiter : autant de passages que nécessaire pour le couvrir
    double order_min = 0;
};

enum class FlightPhase : uint8_t { Parked, Reload, Climb, Cruise, Scoop, Approach, Drop, Exit, Descent };

// La phase est celle du tronçon qui part de ce point.
struct Waypoint {
    double time_min = 0;
    Lambert93 position{};
    FlightPhase phase = FlightPhase::Parked;
};

struct AirDrop {
    double start_min = 0, end_min = 0;
    Lambert93 from{}, to{};
    int order = 0;  // rang dans la liste des ordres
};

struct Flight {
    std::string callsign;
    std::string origin;  // base de départ
    std::string base;    // dernière base : un Dash reste sur le pélicandrome où il s'est rechargé
    AircraftKind kind = AircraftKind::Canadair;
    double first_order_min = 0;  // l'avion n'apparaît qu'à partir de cette heure
    std::vector<Waypoint> track;
    std::vector<AirDrop> drops;

    struct State {
        Lambert93 position{};
        FlightPhase phase = FlightPhase::Parked;
    };
    State at(double time_min) const;
};

// Pas de vol de nuit : les opérations s'arrêtent au coucher du soleil.
class Daylight {
public:
    // Heure locale du départ du feu. utc_offset_h : 2 en été en Corse, 1 en hiver.
    Daylight(int day_of_year, double start_clock_min, double utc_offset_h, const LatLon& where);
    // Vol possible de t0 à t1 ? En minutes depuis le départ du feu.
    bool allows(double t0, double t1) const;
    // Prochain lever du soleil à partir de t.
    double next_sunrise(double t) const;

    double sunrise_clock_min(int day) const;  // minutes depuis minuit, heure locale ; day : jours après le départ
    double sunset_clock_min(int day) const;

private:
    double half_day_min(int day) const;
    double solar_noon_min(int day) const;
    int day_of_year_;
    double start_clock_min_, utc_offset_h_;
    LatLon where_;
};

// Rang des passages : les ordres sont servis par ordre chronologique, chacun par l'avion libre qui largue le plus tôt.
std::vector<Flight> plan_air_attack(const Landscape& land, std::span<const DropOrder> orders,
                                    const std::optional<Daylight>& daylight);

// Nombre de passages nécessaires pour couvrir un axe.
int drop_passes(const DropOrder& order);

// --- Effet sur le feu ---

// Cellules d'un tracé, reliées par côté ou par coin, sans celles qui sortent de la carte.
std::vector<uint32_t> cells_along(const GridInfo& grid, std::span<const Lambert93> path);

// Rang de chaque pare-feu = valeur de Treatment::line, et de SpreadResult::lines.
std::vector<Treatment> treatments_of(const GridInfo& grid, std::span<const Firebreak> firebreaks,
                                     std::span<const Flight> flights);

}  // namespace firesim

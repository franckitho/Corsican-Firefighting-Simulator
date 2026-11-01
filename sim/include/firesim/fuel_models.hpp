#pragma once

#include <array>
#include <string_view>

namespace firesim {

// Modèle de combustible Scott & Burgan (2005), en unités impériales comme les équations de Rothermel.
struct FuelModel {
    int number;                      // code numérique : 101 = GR1, 145 = SH5…
    std::string_view code;
    double depth_ft;                 // épaisseur de la strate combustible
    double mx_dead;                  // humidité d'extinction des combustibles morts (fraction)
    double heat_dead;                // pouvoir calorifique (BTU/lb)
    double heat_live;
    std::array<double, 5> load_tpa;  // charges (t/acre) : 1 h, 10 h, 100 h, herbacé vivant, ligneux vivant
    double sav_1h;                   // rapport surface/volume (1/ft)
    double sav_herb;
    double sav_woody;
    bool dynamic;                    // l'herbe sèche passe des vivants aux morts selon son humidité
};

// nullptr pour les codes non combustibles (0, 91-99) ou inconnus.
const FuelModel* find_fuel_model(int number);

inline bool is_burnable(int number) { return find_fuel_model(number) != nullptr; }

// Sous-bois de forêt (TU, TL) : le vent y est freiné par le couvert.
inline bool is_under_canopy(int number) { return (number >= 161 && number <= 165) || (number >= 181 && number <= 189); }

}  // namespace firesim

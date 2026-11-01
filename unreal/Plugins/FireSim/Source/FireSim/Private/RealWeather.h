#pragma once

#include "CoreMinimal.h"

#include <vector>

#include "firesim/weather.hpp"

// Météo réelle au point de départ d'un feu, heure par heure, en heure locale de la Corse.
struct FRealWeather
{
	FString Source;
	double Latitude = 0, Longitude = 0;
	// Période demandée : SpinUpDays jours avant le feu, puis la durée simulée.
	FDateTime RequestedFirst, RequestedLast;
	// Heures consécutives (« 2025-07-20T13:00 »), relevés et humidité des végétaux morts calculée depuis la première.
	TArray<FString> Times;
	std::vector<firesim::WeatherHour> Hours;
	std::vector<firesim::DeadMoisture> Dead;

	// Rang de cette heure dans les relevés, INDEX_NONE si elle n'y est pas.
	int32 IndexOf(const FDateTime& LocalTime) const;
};

// Téléchargement sur Open-Meteo, comme pipeline/src/firesim_pipeline/meteo.py : archives des prévisions de
// Météo-France (AROME 1,3 km, AROME 2,5 km, ARPEGE), du 13 novembre 2022 aux prévisions des jours à venir ;
// avant, ou s'il manque des heures, la réanalyse ERA5 (depuis 1940, jusqu'à 5 jours avant aujourd'hui).
namespace FireSimWeather
{
constexpr int32 SpinUpDays = 14;

// Météo autour du départ `Ignition` (heure locale) pour `Hours` heures de feu. `Done` est appelé sur le fil du jeu,
// avec la météo ou, en cas d'échec, un message d'erreur.
void Load(double Latitude, double Longitude, const FDateTime& Ignition, double Hours,
	TFunction<void(TSharedPtr<FRealWeather> Weather, const FString& Error)> Done);
}

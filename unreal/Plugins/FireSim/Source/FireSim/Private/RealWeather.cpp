#include "RealWeather.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
// Grandeurs d'Open-Meteo, dans l'ordre des champs de firesim::WeatherHour.
const TCHAR* const kVariables[] = {TEXT("wind_speed_10m"), TEXT("wind_direction_10m"), TEXT("temperature_2m"),
	TEXT("relative_humidity_2m"), TEXT("shortwave_radiation"), TEXT("precipitation")};
constexpr int32 kVariableCount = UE_ARRAY_COUNT(kVariables);

struct FSource
{
	const TCHAR* Name;
	const TCHAR* Url;
	// Pour chaque grandeur, le premier de ces modèles qui a une valeur ; aucun : modèle par défaut.
	TArray<FString> Models;
};

const TArray<FSource>& Sources()
{
	static const TArray<FSource> List = {
		{TEXT("Météo-France AROME / ARPEGE"), TEXT("https://historical-forecast-api.open-meteo.com/v1/forecast"),
			{TEXT("meteofrance_arome_france_hd"), TEXT("meteofrance_arome_france"), TEXT("meteofrance_arpege_europe")}},
		{TEXT("ERA5 (réanalyse, 30 km)"), TEXT("https://archive-api.open-meteo.com/v1/archive"), {}},
	};
	return List;
}

FString HourKey(const FDateTime& Time)
{
	return Time.ToString(TEXT("%Y-%m-%dT%H:00"));
}

// Une tentative de téléchargement, source après source.
struct FAttempt
{
	double Latitude = 0, Longitude = 0;
	FDateTime Ignition, First, Last;
	int32 Source = 0;
	TFunction<void(TSharedPtr<FRealWeather>, const FString&)> Done;
};

FString RequestUrl(const FSource& Source, const FAttempt& Attempt)
{
	FString Url = FString::Printf(TEXT("%s?latitude=%.4f&longitude=%.4f&timezone=Europe%%2FParis&start_date=%s&end_date=%s&hourly="),
		Source.Url, Attempt.Latitude, Attempt.Longitude, *Attempt.First.ToString(TEXT("%Y-%m-%d")),
		*Attempt.Last.ToString(TEXT("%Y-%m-%d")));
	for (int32 K = 0; K < kVariableCount; ++K)
	{
		Url += (K ? TEXT(",") : TEXT("")) + FString(kVariables[K]);
	}
	if (!Source.Models.IsEmpty())
	{
		Url += TEXT("&models=") + FString::Join(Source.Models, TEXT(","));
	}
	return Url;
}

// Relevés de la réponse : complets jusqu'au départ du feu (sinon nul), puis jusqu'au premier trou
// (la dernière heure connue vaut ensuite).
TSharedPtr<FRealWeather> Parse(const FString& Body, const FSource& Source, const FAttempt& Attempt)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Root) || !Root.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject>* Hourly = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Times = nullptr;
	if (!Root->TryGetObjectField(TEXT("hourly"), Hourly) || !(*Hourly)->TryGetArrayField(TEXT("time"), Times))
	{
		return nullptr;
	}
	TArray<TArray<const TArray<TSharedPtr<FJsonValue>>*>> Columns;
	for (const TCHAR* Variable : kVariables)
	{
		// Avec plusieurs modèles, Open-Meteo suffixe chaque grandeur du nom du modèle.
		TArray<FString> Keys;
		if (Source.Models.IsEmpty())
		{
			Keys.Add(Variable);
		}
		for (const FString& Model : Source.Models)
		{
			Keys.Add(FString::Printf(TEXT("%s_%s"), Variable, *Model));
		}
		auto& Candidates = Columns.AddDefaulted_GetRef();
		for (const FString& Key : Keys)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if ((*Hourly)->TryGetArrayField(Key, Values))
			{
				Candidates.Add(Values);
			}
		}
	}
	auto Value = [&Columns](int32 Variable, int32 Hour, double& Out)
	{
		for (const TArray<TSharedPtr<FJsonValue>>* Values : Columns[Variable])
		{
			if (Values->IsValidIndex(Hour) && (*Values)[Hour].IsValid() && (*Values)[Hour]->Type == EJson::Number)
			{
				Out = (*Values)[Hour]->AsNumber();
				return true;
			}
		}
		return false;
	};

	auto Weather = MakeShared<FRealWeather>();
	const FString IgnitionKey = HourKey(Attempt.Ignition);
	bool bReachedIgnition = false;
	for (int32 Hour = 0; Hour < Times->Num(); ++Hour)
	{
		double V[kVariableCount];
		bool bComplete = true;
		for (int32 K = 0; K < kVariableCount && bComplete; ++K)
		{
			bComplete = Value(K, Hour, V[K]);
		}
		if (!bComplete)
		{
			if (!bReachedIgnition)
			{
				return nullptr;
			}
			break;
		}
		const FString Time = (*Times)[Hour]->AsString();
		bReachedIgnition |= Time == IgnitionKey;
		Weather->Times.Add(Time);
		Weather->Hours.push_back({V[0], V[1], V[2], V[3], V[4], V[5]});
	}
	if (!bReachedIgnition)
	{
		return nullptr;
	}
	Weather->Source = Source.Name;
	Weather->Latitude = Attempt.Latitude;
	Weather->Longitude = Attempt.Longitude;
	Weather->RequestedFirst = Attempt.First;
	Weather->RequestedLast = Attempt.Last;
	Weather->Dead = firesim::dead_fuel_moisture(Weather->Hours);
	return Weather;
}

void Request(TSharedRef<FAttempt> Attempt)
{
	if (!Sources().IsValidIndex(Attempt->Source))
	{
		Attempt->Done(nullptr, FString::Printf(TEXT("pas de météo complète pour le %s (%d jours avant compris)"),
			*Attempt->Ignition.ToString(TEXT("%d/%m/%Y à %H h")), FireSimWeather::SpinUpDays));
		return;
	}
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetURL(RequestUrl(Sources()[Attempt->Source], *Attempt));
	Http->SetVerb(TEXT("GET"));
	Http->SetTimeout(30.f);
	Http->OnProcessRequestComplete().BindLambda([Attempt](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		if (!bConnected || !Response.IsValid())
		{
			Attempt->Done(nullptr, TEXT("Open-Meteo injoignable (connexion à Internet ?)"));
			return;
		}
		if (Response->GetResponseCode() == 200)
		{
			if (TSharedPtr<FRealWeather> Weather = Parse(Response->GetContentAsString(), Sources()[Attempt->Source], *Attempt))
			{
				Attempt->Done(Weather, FString());
				return;
			}
		}
		// Hors de la période de cette source, ou heures manquantes : source suivante
		++Attempt->Source;
		Request(Attempt);
	});
	Http->ProcessRequest();
}
}  // namespace

int32 FRealWeather::IndexOf(const FDateTime& LocalTime) const
{
	return Times.IndexOfByKey(HourKey(LocalTime));
}

void FireSimWeather::Load(double Latitude, double Longitude, const FDateTime& Ignition, double Hours,
	TFunction<void(TSharedPtr<FRealWeather> Weather, const FString& Error)> Done)
{
	const TSharedRef<FAttempt> Attempt = MakeShared<FAttempt>();
	Attempt->Latitude = Latitude;
	Attempt->Longitude = Longitude;
	Attempt->Ignition = Ignition;
	Attempt->First = Ignition - FTimespan::FromDays(SpinUpDays);
	Attempt->Last = Ignition + FTimespan::FromHours(Hours);
	Attempt->Done = MoveTemp(Done);
	Request(Attempt);
}

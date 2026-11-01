#include "FireSimulator.h"

#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#include "CesiumIonRasterOverlay.h"
#include "CesiumSunSky.h"
#include "CesiumWebMapTileServiceRasterOverlay.h"
#include "CollisionQueryParams.h"
#include "Components/DecalComponent.h"
#include "Dom/JsonObject.h"
#include "Algo/Count.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/Layout/SBox.h"

#include "FireSimPanel.h"
#include "FireSuppression.h"
#include "RealWeather.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <vector>

#include "firesim/io.hpp"
#include "firesim/lambert93.hpp"
#include "firesim/spread.hpp"

namespace
{
// Altitude du géoïde au-dessus de l'ellipsoïde en Corse (EGM96) : les altitudes IGN sont au-dessus
// du niveau de la mer, Cesium travaille au-dessus de l'ellipsoïde.
constexpr double kGeoidHeightM = 49.0;
// Marge verticale du décalque autour des altitudes du feu : il doit traverser le terrain partout.
constexpr double kDecalMarginM = 500.0;
// Centre de la Corse, origine de la géoréférence (degrés, mètres).
const FVector kCorsicaCenter(9.05, 42.15, 0.0);
// Photo satellite Bing Maps Aerial, présente sur tous les comptes Cesium ion.
constexpr int64 kBingMapsAerialAssetId = 2;
// Photo aérienne IGN : tuiles chargées en même temps. Avec 20 (le défaut de Cesium), la Géoplateforme a cessé de
// répondre à notre adresse IP après un millier de tuiles en 3 minutes.
constexpr int32 kIgnSimultaneousTileLoads = 4;
// Une tuile de la photo aérienne, demandée pour vérifier que la Géoplateforme répond avant d'activer le calque.
const TCHAR* const kIgnProbeUrl = TEXT("https://data.geopf.fr/wmts?request=GetTile&version=1.0.0&service=WMTS&format=image%2Fjpeg&layer=ORTHOIMAGERY.ORTHOPHOTOS&style=normal&tilematrixset=PM&tilematrix=6&tilerow=22&tilecol=34");
// La météo réelle téléchargée sert pour les départs à moins de 3 km environ de son point.
constexpr double kWeatherReuseDegrees = 0.03;
// Au moins 10 jours de relevés avant le feu, pour l'humidité des branches de 100 h.
constexpr int32 kMinSpinUpHours = 10 * 24;

// Carte chargée une fois (environ 600 Mo) et partagée par tous les simulateurs.
struct FLoadedMap
{
	FString Directory;
	firesim::Landscape Land;
	std::vector<float> Elevation;
	std::unique_ptr<firesim::Simulator> Simulator;
};

TUniquePtr<FLoadedMap> GLoadedMap;

// Lève std::exception si un fichier manque.
FLoadedMap& LoadMap(const FString& Directory)
{
	if (!GLoadedMap || GLoadedMap->Directory != Directory)
	{
		GLoadedMap.Reset();
		auto Map = MakeUnique<FLoadedMap>();
		const std::filesystem::path Dir(*Directory);
		Map->Directory = Directory;
		Map->Land = firesim::Landscape::load(Dir);
		Map->Elevation = firesim::io::read_raw<float>(Dir / "elevation.f32", Map->Land.grid.size());
		Map->Simulator = std::make_unique<firesim::Simulator>(Map->Land);
		GLoadedMap = MoveTemp(Map);
	}
	return *GLoadedMap;
}

const char* MoistureName(EFireMoisture Moisture)
{
	switch (Moisture)
	{
	case EFireMoisture::VeryDry: return "tres_sec";
	case EFireMoisture::Dry: return "sec";
	case EFireMoisture::Moderate: return "modere";
	default: return "extreme";
	}
}

// Couleur selon l'heure d'arrivée (0 = départ, 1 = fin de la simulation) : du jaune pâle au violet sombre,
// comme les aperçus du pipeline (inferno inversé).
FColor ArrivalColor(double T)
{
	static const FColor Stops[] = {
		FColor(252, 255, 164), FColor(249, 142, 9), FColor(188, 55, 84), FColor(87, 16, 110), FColor(20, 11, 52)};
	constexpr int32 Last = static_cast<int32>(UE_ARRAY_COUNT(Stops)) - 1;
	const double Scaled = FMath::Clamp(T, 0.0, 1.0) * Last;
	const int32 I = FMath::Min(static_cast<int32>(Scaled), Last - 1);
	return FMath::Lerp(FLinearColor(Stops[I]), FLinearColor(Stops[I + 1]), static_cast<float>(Scaled - I)).ToFColor(true);
}

// L'heure d'arrivée est rangée dans l'alpha de la texture, sur 254 niveaux (255 = pas brûlé) ;
// le matériau compare l'alpha à ce seuil pour n'afficher que ce qui a brûlé à l'heure affichée.
float AlphaThreshold(double Minutes, double DurationMinutes)
{
	return static_cast<float>((FMath::Clamp(Minutes / DurationMinutes, 0.0, 1.0) * 254.0 + 0.5) / 255.0);
}

UTexture2D* MakeOverlayTexture(int32 Columns, int32 Rows, const TArray<FColor>& Pixels)
{
	UTexture2D* Texture = UTexture2D::CreateTransient(Columns, Rows, PF_B8G8R8A8, NAME_None,
		TConstArrayView64<uint8>(reinterpret_cast<const uint8*>(Pixels.GetData()), static_cast<int64>(Pixels.Num()) * sizeof(FColor)));
	Texture->SRGB = true;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->UpdateResource();
	return Texture;
}

// « 2017-07-24T10:00 » : FDateTime::ParseIso8601 veut aussi les secondes.
bool ParseLocalTime(FString Text, FDateTime& Out)
{
	if (Text.Len() == 16)
	{
		Text += TEXT(":00");
	}
	return FDateTime::ParseIso8601(*Text, Out);
}

EFireMoisture MoistureFromName(const FString& Name)
{
	if (Name == TEXT("tres_sec"))
	{
		return EFireMoisture::VeryDry;
	}
	if (Name == TEXT("sec"))
	{
		return EFireMoisture::Dry;
	}
	return Name == TEXT("modere") ? EFireMoisture::Moderate : EFireMoisture::Extreme;
}

// Contour de la zone réellement brûlée, en bleu ; le matériau du feu le montre avec un seuil fixe (OutlineThreshold).
const FColor kObservedColor(40, 220, 255);
constexpr float kOutlineThreshold = 0.5f;
}  // namespace

AFireSimulator::AFireSimulator()
{
	PrimaryActorTick.bCanEverTick = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Suppression = CreateDefaultSubobject<UFireSuppressionComponent>(TEXT("Lutte"));
	OverlayMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/FireSim/M_FireOverlay.M_FireOverlay")));
	const FDateTime Now = FDateTime::Now();
	IgnitionTime = FDateTime(Now.GetYear(), Now.GetMonth(), Now.GetDay(), Now.GetHour());
}

FString AFireSimulator::ResolvedDataDirectory() const
{
	FString Dir = FPaths::IsRelative(DataDirectory) ? FPaths::Combine(FPaths::ProjectDir(), DataDirectory) : DataDirectory;
	return FPaths::ConvertRelativePathToFull(Dir);
}

void AFireSimulator::BeginPlay()
{
	Super::BeginPlay();
	StartAerialPhoto();
	if (bIgniteOnBeginPlay)
	{
		Simulate();
	}
}

void AFireSimulator::StartAerialPhoto()
{
	UCesiumWebMapTileServiceRasterOverlay* Ortho = nullptr;
	for (TActorIterator<ACesium3DTileset> It(GetWorld()); It && !Ortho; ++It)
	{
		Ortho = It->FindComponentByClass<UCesiumWebMapTileServiceRasterOverlay>();
	}
	if (!Ortho)
	{
		return;
	}
	// Coupé tant que le serveur n'a pas répondu : injoignable, chaque tuile échouerait au bout de 21 s
	Ortho->Deactivate();
	Ortho->SetMaximumSimultaneousTileLoads(kIgnSimultaneousTileLoads);

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetURL(kIgnProbeUrl);
	Http->SetVerb(TEXT("GET"));
	Http->SetTimeout(10.f);
	Http->OnProcessRequestComplete().BindLambda(
		[Weak = TWeakObjectPtr<UCesiumWebMapTileServiceRasterOverlay>(Ortho)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		if (!Weak.IsValid())
		{
			return;
		}
		if (bConnected && Response.IsValid() && Response->GetResponseCode() == 200)
		{
			Weak->Activate(true);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("FireSim : Géoplateforme de l'IGN injoignable, photo aérienne coupée (imagerie Bing seule)"));
		}
	});
	Http->ProcessRequest();
}

void AFireSimulator::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Interface)
	{
		if (UGameViewportClient* Viewport = GetWorld()->GetGameViewport())
		{
			Viewport->RemoveViewportWidgetContent(Interface.ToSharedRef());
		}
		Interface.Reset();
	}
	Super::EndPlay(Reason);
}

void AFireSimulator::Simulate()
{
	IgniteAtLongitudeLatitude(IgnitionLongitude, IgnitionLatitude);
}

void AFireSimulator::Recalculate()
{
	if (bHasFire)
	{
		bKeepDisplayedTime = true;
		Simulate();
		bKeepDisplayedTime = false;
	}
}

bool AFireSimulator::IgniteAtWorldLocation(const FVector& Location)
{
	ACesiumGeoreference* Georeference = ACesiumGeoreference::GetDefaultGeoreferenceForActor(this);
	const FVector Local = Georeference->GetActorTransform().InverseTransformPosition(Location);
	const FVector LonLatHeight = Georeference->TransformUnrealPositionToLongitudeLatitudeHeight(Local);
	Suppression->Reset();  // un autre feu : les ordres donnés pour le précédent ne valent plus
	return IgniteAtLongitudeLatitude(LonLatHeight.X, LonLatHeight.Y);
}

bool AFireSimulator::IgniteAtLongitudeLatitude(double Longitude, double Latitude)
{
	IgnitionLongitude = Longitude;
	IgnitionLatitude = Latitude;
	if (bRealWeather && !RealWeatherCovers(Longitude, Latitude))
	{
		LoadRealWeather();
		return true;
	}
	const double DurationMinutes = DurationHours * 60.0;
	const firesim::Lambert93 Point = firesim::to_lambert93({Latitude, Longitude});
	const int32 WeatherStart = bRealWeather ? RealWeather->IndexOf(IgnitionTime) : INDEX_NONE;

	FLoadedMap* Map = nullptr;
	firesim::SpreadResult Result;
	firesim::SpreadResult FreeResult;  // le même feu sans la lutte, pour comparer
	bool bFought = false;
	const double Start = FPlatformTime::Seconds();
	try
	{
		Map = &LoadMap(ResolvedDataDirectory());
		// Météo réelle : la sécheresse choisie ne règle que la végétation vivante
		const firesim::Moisture Scenario = *firesim::moisture_scenario(MoistureName(Moisture));
		const firesim::Weather Weather = bRealWeather
			? firesim::Weather::from_hours(RealWeather->Hours, static_cast<size_t>(WeatherStart), Scenario.live_herb,
				  Scenario.live_woody)
			: firesim::Weather::constant(WindSpeedKmh, WindFromDegrees, Scenario);
		firesim::SpreadOptions Options;
		Options.duration_min = DurationMinutes;
		Options.terrain_wind = bTerrainWind;
		const firesim::Ignition Ignition{Point.x, Point.y, 0.0};
		// Pas de vol de nuit : le lever et le coucher du soleil ne comptent que si le feu a une date (météo réelle)
		std::optional<firesim::Daylight> Daylight;
		if (bRealWeather)
		{
			const double UtcOffset = IgnitionTime.GetMonth() >= 4 && IgnitionTime.GetMonth() <= 10 ? 2.0 : 1.0;
			Daylight.emplace(IgnitionTime.GetDayOfYear(), IgnitionTime.GetHour() * 60.0 + IgnitionTime.GetMinute(), UtcOffset,
				firesim::LatLon{Latitude, Longitude});
		}
		Options.treatments = Suppression->Prepare(Map->Land, Map->Elevation, Options.breach_factor, Daylight);
		bFought = !Options.treatments.empty();
		Result = Map->Simulator->run(Weather, {&Ignition, 1}, Options);
		if (bFought)
		{
			Options.treatments.clear();
			FreeResult = Map->Simulator->run(Weather, {&Ignition, 1}, Options);
		}
	}
	catch (const std::exception& Error)
	{
		Status = FString::Printf(TEXT("Échec : %s"), UTF8_TO_TCHAR(Error.what()));
		UE_LOG(LogTemp, Warning, TEXT("FireSim : %s"), *Status);
		return false;
	}
	const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	// Texture sur l'emprise du feu : ligne 0 au nord, colonne 0 à l'ouest, comme la grille.
	const firesim::GridInfo& Grid = Map->Land.grid;
	FFireDecalWindow& W = FireWindow;
	W.Columns = Result.col_max - Result.col_min + 1;
	W.Rows = Result.row_max - Result.row_min + 1;
	W.West = Grid.left + Result.col_min * Grid.res;
	W.North = Grid.top - Result.row_min * Grid.res;
	W.CellSize = Grid.res;
	SimulatedMinutes = DurationMinutes;

	TArray<FColor> Pixels;
	Pixels.Init(FColor(0, 0, 0, 255), W.Columns * W.Rows);
	ArrivalMinutes.Reset(static_cast<int32>(Result.cells.size()));
	BurnedCells.Reset(static_cast<int32>(Result.cells.size()));
	W.MinElevation = TNumericLimits<double>::Max();
	W.MaxElevation = TNumericLimits<double>::Lowest();
	MaxSpreadKmh = 0;
	MaxFlameLengthM = 0;
	for (const firesim::BurnedCell& Cell : Result.cells)
	{
		const int32 Col = static_cast<int32>(Cell.index % Grid.width) - Result.col_min;
		const int32 Row = static_cast<int32>(Cell.index / Grid.width) - Result.row_min;
		const double T = Cell.arrival_min / DurationMinutes;
		FColor Color = ArrivalColor(T);
		Color.A = static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(T, 0.0, 1.0) * 254.0));
		Pixels[Row * W.Columns + Col] = Color;
		ArrivalMinutes.Add(Cell.arrival_min);
		BurnedCells.Add(static_cast<int64>(Cell.index));
		if (const float Elevation = Map->Elevation[Cell.index]; FMath::IsFinite(Elevation))
		{
			W.MinElevation = FMath::Min(W.MinElevation, static_cast<double>(Elevation));
			W.MaxElevation = FMath::Max(W.MaxElevation, static_cast<double>(Elevation));
		}
		MaxSpreadKmh = FMath::Max(MaxSpreadKmh, Cell.ros_m_min * 0.06);
		MaxFlameLengthM = FMath::Max(MaxFlameLengthM, static_cast<double>(Cell.flame_m));
	}
	if (W.MinElevation > W.MaxElevation)
	{
		W.MinElevation = W.MaxElevation = 0;
	}
	BurnedAreaHa = Result.area_ha;
	Status = FString::Printf(TEXT("%.0f ha en %.1f h, calcul %.0f ms%s"), BurnedAreaHa, DurationHours, ElapsedMs,
		Result.reached_grid_edge ? TEXT(" (bord de la carte atteint)") : TEXT(""));
	UE_LOG(LogTemp, Display, TEXT("FireSim : %s"), *Status);

	bHasFire = true;
	FireWeather = bRealWeather ? RealWeather : nullptr;
	FireWeatherStart = WeatherStart;
	FireStartTime = IgnitionTime;
	UpdateReplayOverlap();
	ShowFire(Pixels);
	Suppression->SetResult(Result, bFought ? &FreeResult : nullptr, DurationMinutes);
	UWorld* World = GetWorld();
	if (World && World->IsGameWorld() && !bKeepDisplayedTime)
	{
		DisplayedHours = 0;
		bPlaying = true;
	}
	UpdateDisplayedTime();
	return true;
}

void AFireSimulator::ShowFire(const TArray<FColor>& Pixels)
{
	FireTexture = MakeOverlayTexture(FireWindow.Columns, FireWindow.Rows, Pixels);
	if (!OverlayInstance)
	{
		UMaterialInterface* Parent = OverlayMaterial.LoadSynchronous();
		if (!Parent)
		{
			Status = TEXT("Matériau du calque introuvable : lancer Scripts/create_assets.py");
			UE_LOG(LogTemp, Warning, TEXT("FireSim : %s"), *Status);
			return;
		}
		OverlayInstance = UMaterialInstanceDynamic::Create(Parent, this);
	}
	OverlayInstance->SetTextureParameterValue(TEXT("FireTexture"), FireTexture);
	OverlayInstance->SetScalarParameterValue(TEXT("FrontWidth"), static_cast<float>(FrontMinutes / SimulatedMinutes * 254.0 / 255.0));

	if (!Decal)
	{
		Decal = MakeDecal(TEXT("FireDecal"));
	}
	Decal->SetDecalMaterial(OverlayInstance);
	Decal->SetVisibility(true);
	PlaceDecal(Decal, FireWindow);
}

UDecalComponent* AFireSimulator::MakeDecal(FName Name)
{
	UDecalComponent* NewDecal = NewObject<UDecalComponent>(this, Name, RF_Transient);
	NewDecal->SetupAttachment(Root);
	NewDecal->SetFadeScreenSize(0.f);
	NewDecal->RegisterComponent();
	ACesiumGeoreference* Georeference = ACesiumGeoreference::GetDefaultGeoreferenceForActor(this);
	Georeference->OnGeoreferenceUpdated.AddUniqueDynamic(this, &AFireSimulator::HandleGeoreferenceUpdated);
	return NewDecal;
}

void AFireSimulator::PlaceDecal(UDecalComponent* Target, const FFireDecalWindow& Window)
{
	if (!Target || Window.Columns == 0)
	{
		return;
	}
	ACesiumGeoreference* Georeference = ACesiumGeoreference::GetDefaultGeoreferenceForActor(this);
	const FTransform GeoreferenceToWorld = Georeference->GetActorTransform();
	auto ToWorld = [&](double X, double Y, double Height)
	{
		const firesim::LatLon P = firesim::from_lambert93({X, Y});
		return GeoreferenceToWorld.TransformPosition(
			Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(P.lon, P.lat, Height)));
	};

	// Coins de l'emprise, à l'altitude moyenne couverte : la conversion tient compte de la rotation
	// de la grille Lambert 93 (convergence des méridiens) et de la courbure de la Terre.
	const double Height = (Window.MinElevation + Window.MaxElevation) / 2.0 + kGeoidHeightM;
	const double West = Window.West, North = Window.North;
	const double East = West + Window.Columns * Window.CellSize, South = North - Window.Rows * Window.CellSize;
	const FVector NorthWest = ToWorld(West, North, Height), NorthEast = ToWorld(East, North, Height);
	const FVector SouthWest = ToWorld(West, South, Height), SouthEast = ToWorld(East, South, Height);
	const FVector Center = (NorthWest + NorthEast + SouthWest + SouthEast) / 4.0;
	const FVector AcrossColumns = (NorthEast - NorthWest + SouthEast - SouthWest) / 2.0;
	const FVector AcrossRows = (SouthWest - NorthWest + SouthEast - NorthEast) / 2.0;
	const double CenterX = (West + East) / 2.0, CenterY = (North + South) / 2.0;
	const FVector UpPerMeter = (ToWorld(CenterX, CenterY, Height + 1000.0) - ToWorld(CenterX, CenterY, Height)) / 1000.0;

	// Le décalque projette selon son axe X (vers le bas) ; sa texture suit Z (U, vers l'est) et Y (V, vers le sud).
	const FRotator Rotation = FRotationMatrix::MakeFromXZ(-UpPerMeter, AcrossColumns).Rotator();
	const double HalfDepthM = (Window.MaxElevation - Window.MinElevation) / 2.0 + kDecalMarginM;
	Target->DecalSize = FVector(HalfDepthM * UpPerMeter.Size(), AcrossRows.Size() / 2.0, AcrossColumns.Size() / 2.0);
	Target->SetWorldLocationAndRotation(Center, Rotation);
	Target->MarkRenderStateDirty();
}

void AFireSimulator::HandleGeoreferenceUpdated()
{
	if (bHasFire)
	{
		PlaceDecal(Decal, FireWindow);
	}
	PlaceDecal(ObservedDecal, ObservedWindow);
	Suppression->PlaceOverlays();
}

void AFireSimulator::UpdateDisplayedTime()
{
	if (OverlayInstance && bHasFire)
	{
		OverlayInstance->SetScalarParameterValue(TEXT("Threshold"), AlphaThreshold(DisplayedHours * 60.0, SimulatedMinutes));
	}
}

bool AFireSimulator::RealWeatherCovers(double Longitude, double Latitude) const
{
	return RealWeather && FMath::Abs(RealWeather->Latitude - Latitude) < kWeatherReuseDegrees &&
		   FMath::Abs(RealWeather->Longitude - Longitude) < kWeatherReuseDegrees &&
		   RealWeather->IndexOf(IgnitionTime) >= kMinSpinUpHours &&
		   RealWeather->RequestedLast >= IgnitionTime + FTimespan::FromHours(DurationHours);
}

void AFireSimulator::LoadRealWeather()
{
	if (bLoadingWeather)
	{
		return;  // la météo en cours de chargement relancera le calcul avec les derniers réglages
	}
	bLoadingWeather = true;
	Status = TEXT("Chargement de la météo…");
	TWeakObjectPtr<AFireSimulator> Self(this);
	FireSimWeather::Load(IgnitionLatitude, IgnitionLongitude, IgnitionTime, DurationHours,
		[Self](TSharedPtr<FRealWeather> Weather, const FString& Error)
		{
			AFireSimulator* Simulator = Self.Get();
			if (!Simulator)
			{
				return;
			}
			Simulator->bLoadingWeather = false;
			if (!Weather)
			{
				Simulator->Status = FString::Printf(TEXT("Météo : %s"), *Error);
				UE_LOG(LogTemp, Warning, TEXT("FireSim : %s"), *Simulator->Status);
				return;
			}
			Simulator->RealWeather = Weather;
			Simulator->WeatherSource = Weather->Source;
			UE_LOG(LogTemp, Display, TEXT("FireSim : météo %s, %d heures depuis le %s"), *Weather->Source, Weather->Times.Num(),
				*Weather->Times[0]);
			if (Simulator->bRealWeather)
			{
				Simulator->Simulate();
			}
		});
}

bool AFireSimulator::RealWeatherAt(double FireHours, FFireWeatherNow& Out) const
{
	if (!bHasFire || !FireWeather || FireWeatherStart == INDEX_NONE)
	{
		return false;
	}
	// Chaque relevé vaut pour l'heure qui suit ; après le dernier, il reste valable.
	const int32 Index = FMath::Min(FireWeatherStart + FMath::Max(0, FMath::FloorToInt32(FireHours)),
		static_cast<int32>(FireWeather->Hours.size()) - 1);
	const firesim::WeatherHour& Hour = FireWeather->Hours[Index];
	Out.Time = FireStartTime + FTimespan::FromHours(FireHours);
	Out.WindKmh = Hour.wind_kmh;
	Out.WindFromDeg = Hour.wind_from_deg;
	Out.TemperatureC = Hour.temperature_c;
	Out.HumidityPct = Hour.humidity_pct;
	Out.Dead1h = FireWeather->Dead[Index].d1h;
	return true;
}

void AFireSimulator::SetPlaying(bool bPlay)
{
	if (bPlay && DisplayedHours >= GetSimulatedHours())
	{
		SetDisplayedHours(0);
	}
	bPlaying = bPlay && bHasFire;
}

void AFireSimulator::SetDisplayedHours(double Hours)
{
	DisplayedHours = FMath::Clamp(Hours, 0.0, GetSimulatedHours());
	UpdateDisplayedTime();
}

int32 AFireSimulator::BurnedCountAt(double Hours) const
{
	// Les cellules sont rangées dans l'ordre d'arrivée du feu.
	const float Minutes = static_cast<float>(Hours * 60.0);
	return static_cast<int32>(
		std::upper_bound(ArrivalMinutes.GetData(), ArrivalMinutes.GetData() + ArrivalMinutes.Num(), Minutes) - ArrivalMinutes.GetData());
}

double AFireSimulator::AreaAt(double Hours) const
{
	return BurnedCountAt(Hours) * FireWindow.CellSize * FireWindow.CellSize / 10000.0;
}

void AFireSimulator::LoadReplays()
{
	Replays.Reset();
	const FString Directory = FPaths::Combine(ResolvedDataDirectory(), TEXT(".."), TEXT("feux"));
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
	Files.Sort();
	for (const FString& File : Files)
	{
		FString Text;
		TSharedPtr<FJsonObject> Json;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Directory, File)) ||
			!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* Contour = nullptr;
		FFireReplay Replay;
		FString Start, MaskFile;
		if (!Json->TryGetStringField(TEXT("id"), Replay.Id) || !Json->TryGetStringField(TEXT("nom"), Replay.Name) ||
			!Json->TryGetStringField(TEXT("depart"), Start) || !ParseLocalTime(Start, Replay.Start) ||
			!Json->TryGetObjectField(TEXT("contour"), Contour) || !(*Contour)->TryGetStringField(TEXT("fichier"), MaskFile))
		{
			UE_LOG(LogTemp, Warning, TEXT("FireSim : feu réel illisible : %s"), *File);
			continue;
		}
		if (!Json->TryGetStringField(TEXT("nom_court"), Replay.ShortName))
		{
			Replay.ShortName = Replay.Name;
		}
		Replay.Latitude = Json->GetNumberField(TEXT("latitude"));
		Replay.Longitude = Json->GetNumberField(TEXT("longitude"));
		Replay.DurationHours = Json->GetNumberField(TEXT("duree_h"));
		Replay.ObservedHa = Json->GetNumberField(TEXT("observe_ha"));
		Replay.LiveMoisture = MoistureFromName(Json->GetStringField(TEXT("vegetation_vivante")));
		Replay.Col = (*Contour)->GetIntegerField(TEXT("col"));
		Replay.Row = (*Contour)->GetIntegerField(TEXT("row"));
		Replay.Width = (*Contour)->GetIntegerField(TEXT("width"));
		Replay.Height = (*Contour)->GetIntegerField(TEXT("height"));
		if (!FFileHelper::LoadFileToArray(Replay.Mask, *FPaths::Combine(Directory, MaskFile)) ||
			Replay.Mask.Num() != Replay.Width * Replay.Height)
		{
			UE_LOG(LogTemp, Warning, TEXT("FireSim : contour du feu réel illisible : %s"), *MaskFile);
			continue;
		}
		Replay.ObservedCells = static_cast<int32>(Algo::CountIf(Replay.Mask, [](uint8 Value) { return Value != 0; }));
		const TArray<TSharedPtr<FJsonValue>>* Reported = nullptr;
		if (Json->TryGetArrayField(TEXT("observations"), Reported))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Reported)
			{
				const TSharedPtr<FJsonObject> Observation = Value->AsObject();
				FDateTime When;
				if (Observation && ParseLocalTime(Observation->GetStringField(TEXT("heure")), When))
				{
					Replay.Reported.Emplace(When, Observation->GetNumberField(TEXT("ha")));
				}
			}
		}
		Replays.Add(MoveTemp(Replay));
	}
	// Les plus récents d'abord
	Replays.Sort([](const FFireReplay& A, const FFireReplay& B) { return A.Start > B.Start; });
}

bool AFireSimulator::StartReplay(const FString& Id)
{
	if (Replays.IsEmpty())
	{
		LoadReplays();
	}
	const int32 Index = Replays.IndexOfByPredicate([&Id](const FFireReplay& Replay) { return Replay.Id == Id; });
	if (Index == INDEX_NONE)
	{
		Status = FString::Printf(TEXT("Feu réel inconnu : %s (lancer firesim_pipeline.rejeu)"), *Id);
		return false;
	}
	ActiveReplay = Index;
	Suppression->Reset();
	const FFireReplay& Replay = Replays[Index];
	bRealWeather = true;
	IgnitionTime = Replay.Start;
	DurationHours = Replay.DurationHours;
	Moisture = Replay.LiveMoisture;
	ShowObservedArea(Replay);
	return IgniteAtLongitudeLatitude(Replay.Longitude, Replay.Latitude);
}

void AFireSimulator::ShowObservedArea(const FFireReplay& Replay)
{
	const FLoadedMap* Map = nullptr;
	try
	{
		Map = &LoadMap(ResolvedDataDirectory());
	}
	catch (const std::exception& Error)
	{
		Status = FString::Printf(TEXT("Échec : %s"), UTF8_TO_TCHAR(Error.what()));
		return;
	}
	const firesim::GridInfo& Grid = Map->Land.grid;
	FFireDecalWindow& W = ObservedWindow;
	W.Columns = Replay.Width;
	W.Rows = Replay.Height;
	W.West = Grid.left + Replay.Col * Grid.res;
	W.North = Grid.top - Replay.Row * Grid.res;
	W.CellSize = Grid.res;
	W.MinElevation = TNumericLimits<double>::Max();
	W.MaxElevation = TNumericLimits<double>::Lowest();

	// Seul le bord est visible : alpha 0 sous le seuil du matériau, 255 au-dessus.
	TArray<FColor> Pixels;
	Pixels.Init(FColor(0, 0, 0, 255), Replay.Width * Replay.Height);
	for (int32 Row = 0; Row < Replay.Height; ++Row)
	{
		for (int32 Col = 0; Col < Replay.Width; ++Col)
		{
			const int32 I = Row * Replay.Width + Col;
			if (Replay.Mask[I] == 0)
			{
				continue;
			}
			if (Replay.Mask[I] == 2)
			{
				Pixels[I] = FColor(kObservedColor.R, kObservedColor.G, kObservedColor.B, 0);
			}
			const int64 Cell = static_cast<int64>(Replay.Row + Row) * Grid.width + Replay.Col + Col;
			if (const float Elevation = Map->Elevation[Cell]; FMath::IsFinite(Elevation))
			{
				W.MinElevation = FMath::Min(W.MinElevation, static_cast<double>(Elevation));
				W.MaxElevation = FMath::Max(W.MaxElevation, static_cast<double>(Elevation));
			}
		}
	}
	if (W.MinElevation > W.MaxElevation)
	{
		W.MinElevation = W.MaxElevation = 0;
	}
	ObservedTexture = MakeOverlayTexture(Replay.Width, Replay.Height, Pixels);
	if (!ObservedInstance)
	{
		UMaterialInterface* Parent = OverlayMaterial.LoadSynchronous();
		if (!Parent)
		{
			return;
		}
		ObservedInstance = UMaterialInstanceDynamic::Create(Parent, this);
		ObservedInstance->SetScalarParameterValue(TEXT("Threshold"), kOutlineThreshold);
		ObservedInstance->SetScalarParameterValue(TEXT("FrontWidth"), 1e-4f);  // pas de front : la couleur de la texture
		ObservedInstance->SetScalarParameterValue(TEXT("Opacity"), 0.95f);
	}
	ObservedInstance->SetTextureParameterValue(TEXT("FireTexture"), ObservedTexture);
	if (!ObservedDecal)
	{
		ObservedDecal = MakeDecal(TEXT("ObservedDecal"));
		ObservedDecal->SetSortOrder(1);  // par-dessus le feu simulé
	}
	ObservedDecal->SetDecalMaterial(ObservedInstance);
	ObservedDecal->SetVisibility(true);
	PlaceDecal(ObservedDecal, W);
}

void AFireSimulator::UpdateReplayOverlap()
{
	ObservedPrefix.Reset();
	const FFireReplay* Replay = GetActiveReplay();
	if (!Replay || !GLoadedMap)
	{
		return;
	}
	const int64 GridWidth = GLoadedMap->Land.grid.width;
	ObservedPrefix.Reserve(BurnedCells.Num() + 1);
	ObservedPrefix.Add(0);
	for (const int64 Cell : BurnedCells)
	{
		const int32 Col = static_cast<int32>(Cell % GridWidth) - Replay->Col;
		const int32 Row = static_cast<int32>(Cell / GridWidth) - Replay->Row;
		const bool bInside = Col >= 0 && Row >= 0 && Col < Replay->Width && Row < Replay->Height &&
							 Replay->Mask[Row * Replay->Width + Col] != 0;
		ObservedPrefix.Add(ObservedPrefix.Last() + (bInside ? 1 : 0));
	}
}

bool AFireSimulator::ReplayScoresAt(double FireHours, double& SimulatedHa, double& Sorensen) const
{
	const FFireReplay* Replay = GetActiveReplay();
	if (!Replay || !bHasFire || ObservedPrefix.Num() != BurnedCells.Num() + 1)
	{
		return false;
	}
	const int32 Burned = BurnedCountAt(FireHours);
	SimulatedHa = Burned * FireWindow.CellSize * FireWindow.CellSize / 10000.0;
	Sorensen = 2.0 * ObservedPrefix[Burned] / FMath::Max(1, Burned + Replay->ObservedCells);
	return true;
}

void AFireSimulator::ClearFire()
{
	bHasFire = false;
	bPlaying = false;
	Suppression->Reset();
	ArrivalMinutes.Reset();
	BurnedCells.Reset();
	ObservedPrefix.Reset();
	FireWeather.Reset();
	FireWeatherStart = INDEX_NONE;
	ActiveReplay = INDEX_NONE;
	if (ObservedDecal)
	{
		ObservedDecal->SetVisibility(false);
	}
	BurnedAreaHa = MaxSpreadKmh = MaxFlameLengthM = 0;
	Status.Reset();
	if (Decal)
	{
		Decal->SetVisibility(false);
	}
}

void AFireSimulator::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
	{
		if (!Interface)
		{
			CreateInterface(*Controller);
		}
		if (Controller->WasInputKeyJustPressed(ToggleInterfaceKey))
		{
			bShowInterface = !bShowInterface;
			ApplyInputMode(*Controller);
		}
		if (Controller->WasInputKeyJustPressed(IgniteKey))
		{
			IgniteUnderCursor(*Controller);
		}
		Suppression->HandleInput(*Controller);
	}

	if (bHasFire && bPlaying)
	{
		// Durée du feu calculé : le curseur de durée peut avoir bougé depuis
		const double End = GetSimulatedHours();
		DisplayedHours = FMath::Min(DisplayedHours + DeltaSeconds * Suppression->AdjustSpeed(PlaybackSpeed), End);
		bPlaying = DisplayedHours < End;
		UpdateDisplayedTime();
	}
}

void AFireSimulator::CreateInterface(APlayerController& Controller)
{
	UGameViewportClient* Viewport = GetWorld()->GetGameViewport();
	if (!Viewport)
	{
		return;
	}
	LoadReplays();  // le panneau propose les feux réels du catalogue
	TWeakObjectPtr<AFireSimulator> Self(this);
	Interface = SNew(SBox)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(16.f)
		.Visibility_Lambda([Self]
		{
			return Self.IsValid() && Self->bShowInterface ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed;
		})
		[
			SNew(SFireSimPanel, this)
		];
	Viewport->AddViewportWidgetContent(Interface.ToSharedRef(), 10);
	Suppression->CreateInterface(*Viewport);
	ApplyInputMode(Controller);
}

void AFireSimulator::ApplyInputMode(APlayerController& Controller) const
{
	Controller.SetShowMouseCursor(bShowInterface);
	if (bShowInterface)
	{
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(true);
		Controller.SetInputMode(Mode);
	}
	else
	{
		Controller.SetInputMode(FInputModeGameOnly());
	}
}

void AFireSimulator::IgniteUnderCursor(APlayerController& Controller)
{
	FVector Eye, Direction;
	if (!Controller.ShouldShowMouseCursor() || !Controller.DeprojectMousePositionToWorld(Eye, Direction))
	{
		FRotator View;
		Controller.GetPlayerViewPoint(Eye, View);
		Direction = View.Vector();
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FireSimIgnite), true, Controller.GetPawn());
	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Direction * 5.0e7, ECC_Visibility, Params))
	{
		IgniteAtWorldLocation(Hit.ImpactPoint);
	}
}

#if WITH_EDITOR
void AFireSimulator::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	const FName Name = Event.GetMemberPropertyName();
	if (Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, DisplayedHours))
	{
		UpdateDisplayedTime();
	}
	else if (bHasFire &&
			 (Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, IgnitionLatitude) ||
				 Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, IgnitionLongitude) ||
				 Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, WindSpeedKmh) ||
				 Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, WindFromDegrees) ||
				 Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, Moisture) ||
				 Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, bTerrainWind) ||
				 Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, DurationHours) ||
				 Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, bRealWeather) ||
				 Name == GET_MEMBER_NAME_CHECKED(AFireSimulator, IgnitionTime)))
	{
		Simulate();  // 0,1 à 0,2 s : on recalcule à chaque réglage
	}
}
#endif

void AFireSimulator::PrepareScene()
{
	UWorld* World = GetWorld();
	ACesiumGeoreference* Georeference = ACesiumGeoreference::GetDefaultGeoreferenceForActor(this);
	Georeference->SetOriginLongitudeLatitudeHeight(kCorsicaCenter);
	// Recommandé par Cesium : à l'échelle du globe, ces limites détruiraient des acteurs valides
	World->GetWorldSettings()->bEnableWorldBoundsChecks = false;

	int64 AssetId = TerrainIonAssetId;
	if (AssetId == 0)
	{
		const FString Path = FPaths::Combine(ResolvedDataDirectory(), TEXT(".."), TEXT("terrain"), TEXT("ion_asset.json"));
		FString Text;
		TSharedPtr<FJsonObject> Json;
		if (FFileHelper::LoadFileToString(Text, *Path) &&
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid())
		{
			AssetId = static_cast<int64>(Json->GetNumberField(TEXT("asset_id")));
		}
	}

	// Terrain : le relief IGN envoyé sur Cesium ion (étape « ion » du pipeline)
	ACesium3DTileset* Terrain = nullptr;
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
	{
		if (It->GetIonAssetID() == AssetId)
		{
			Terrain = *It;
		}
	}
	if (!Terrain && AssetId != 0)
	{
		Terrain = World->SpawnActor<ACesium3DTileset>();
		Terrain->SetTilesetSource(ETilesetSource::FromCesiumIon);
		Terrain->SetIonAssetID(AssetId);
#if WITH_EDITOR
		Terrain->SetActorLabel(TEXT("Terrain Corse (IGN 5 m)"));
#endif
	}

	// Imagerie : Cesium empile les calques selon leur clé, Overlay1 par-dessus Overlay0. Dessous, Bing (monde entier,
	// via Cesium ion) ; dessus, la photo aérienne IGN, qui ne couvre que la France.
	auto AddOverlay = [Terrain](UCesiumRasterOverlay* Overlay, const TCHAR* Key)
	{
		Overlay->MaterialLayerKey = Key;
		Overlay->CreationMethod = EComponentCreationMethod::Instance;
		Terrain->AddInstanceComponent(Overlay);
		Overlay->RegisterComponent();
	};
	if (Terrain && !Terrain->FindComponentByClass<UCesiumIonRasterOverlay>())
	{
		auto* Global = NewObject<UCesiumIonRasterOverlay>(Terrain, TEXT("ImagerieMondeBing"));
		Global->IonAssetID = kBingMapsAerialAssetId;
		AddOverlay(Global, TEXT("Overlay0"));
	}
	if (Terrain)
	{
		if (auto* Ortho = Terrain->FindComponentByClass<UCesiumWebMapTileServiceRasterOverlay>())
		{
			Ortho->MaterialLayerKey = TEXT("Overlay1");
			Ortho->SetMaximumSimultaneousTileLoads(kIgnSimultaneousTileLoads);
			Ortho->Refresh();
		}
		else
		{
			// Géoplateforme de l'IGN, flux WMTS en Web Mercator
			Ortho = NewObject<UCesiumWebMapTileServiceRasterOverlay>(Terrain, TEXT("PhotoAerienneIGN"));
			Ortho->BaseUrl = TEXT("https://data.geopf.fr/wmts");
			Ortho->Layer = TEXT("ORTHOIMAGERY.ORTHOPHOTOS");
			Ortho->Style = TEXT("normal");
			Ortho->Format = TEXT("image/jpeg");
			Ortho->TileMatrixSetID = TEXT("PM");
			Ortho->Projection = ECesiumWebMapTileServiceRasterOverlayProjection::WebMercator;
			Ortho->bSpecifyZoomLevels = true;
			Ortho->MinimumLevel = 0;
			Ortho->MaximumLevel = 19;
			Ortho->SetMaximumSimultaneousTileLoads(kIgnSimultaneousTileLoads);
			AddOverlay(Ortho, TEXT("Overlay1"));
		}
	}

	// Soleil et ciel
	if (!TActorIterator<ACesiumSunSky>(World))
	{
		World->SpawnActor<ACesiumSunSky>();
	}

	// Caméra de Cesium (vol libre, vitesse adaptée à l'altitude), au sud-ouest du départ, tournée vers le nord-est
	UClass* PawnClass = LoadClass<APawn>(nullptr, TEXT("/CesiumForUnreal/DynamicPawn.DynamicPawn_C"));
	if (PawnClass && !TActorIterator<APawn>(World))
	{
		const FTransform GeoreferenceToWorld = Georeference->GetActorTransform();
		const FVector Local = Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(
			FVector(IgnitionLongitude - 0.04, IgnitionLatitude - 0.03, 2500.0));
		// Repère est-sud-haut : lacet -45° = vers le nord-est, tangage -30° = vers le sol
		const FRotator LocalLook = Georeference->TransformEastSouthUpRotatorToUnreal(FRotator(-30.0, -45.0, 0.0), Local);
		const FVector Eye = GeoreferenceToWorld.TransformPosition(Local);
		const FRotator Look = GeoreferenceToWorld.TransformRotation(LocalLook.Quaternion()).Rotator();
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (APawn* Pawn = World->SpawnActor<APawn>(PawnClass, Eye, Look, Params))
		{
			Pawn->AutoPossessPlayer = EAutoReceiveInput::Player0;
		}
	}
	Status = AssetId != 0 ? FString::Printf(TEXT("Scène prête, terrain ion %lld"), AssetId)
						  : FString(TEXT("Scène prête, mais pas d'asset de terrain : lancer l'étape « ion » du pipeline"));
}

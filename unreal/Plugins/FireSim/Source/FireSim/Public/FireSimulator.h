#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "FireSimulator.generated.h"

class APlayerController;
class SWidget;
class UDecalComponent;
class UFireSuppressionComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTexture2D;
struct FRealWeather;

// Météo réelle à un instant du feu, pour l'affichage.
struct FFireWeatherNow
{
	FDateTime Time;  // heure locale
	double WindKmh = 0, WindFromDeg = 0, TemperatureC = 0, HumidityPct = 0;
	double Dead1h = 0;  // humidité des brindilles mortes (fraction)
};

// Emprise d'un calque posé sur le terrain : cellules de la grille Lambert 93 et altitudes qu'il recouvre.
struct FFireDecalWindow
{
	double West = 0, North = 0, CellSize = 0;
	int32 Columns = 0, Rows = 0;
	double MinElevation = 0, MaxElevation = 0;
};

// Scénarios d'humidité des combustibles de Scott & Burgan, du plus sec au plus humide.
UENUM(BlueprintType)
enum class EFireMoisture : uint8
{
	Extreme UMETA(DisplayName = "Extrême (D1L1)"),
	VeryDry UMETA(DisplayName = "Très sec (D2L2)"),
	Dry UMETA(DisplayName = "Sec (D3L3)"),
	Moderate UMETA(DisplayName = "Modéré (D4L4)"),
};

// Feu réel du catalogue (data/processed/feux, écrit par firesim_pipeline.rejeu), avec la zone réellement brûlée.
struct FFireReplay
{
	FString Id, Name, ShortName;
	FDateTime Start;  // heure locale du départ
	double Latitude = 0, Longitude = 0, DurationHours = 0, ObservedHa = 0;
	EFireMoisture LiveMoisture = EFireMoisture::Extreme;
	// Zone brûlée observée sur la grille de simulation, à partir de (Col, Row) : 0 dehors, 1 brûlé, 2 bord.
	int32 Col = 0, Row = 0, Width = 0, Height = 0;
	TArray<uint8> Mask;
	int32 ObservedCells = 0;
	// Surfaces annoncées pendant le feu (heure locale, hectares).
	TArray<TPair<FDateTime, double>> Reported;
};

// Simulateur de feu posé dans la carte : départ de feu, calcul de la propagation avec le module sim/
// du dépôt, et affichage des zones brûlées sur le terrain Cesium (décalque coloré par heure d'arrivée).
//
// Dans l'éditeur : bouton « Simulate » des détails ; « Displayed Hours » fait défiler le feu.
// En jeu : panneau de réglage (vent, végétation, durée, lecture) et touche F pour allumer un feu sous la souris.
UCLASS()
class FIRESIM_API AFireSimulator : public AActor
{
	GENERATED_BODY()

public:
	AFireSimulator();

	// Couches produites par le pipeline (data/processed/sim) : chemin absolu ou relatif au dossier du projet.
	UPROPERTY(EditAnywhere, Category = "Feu|Carte")
	FString DataDirectory = TEXT("../data/processed/sim");

	// Asset du terrain sur Cesium ion ; 0 = celui de data/processed/terrain/ion_asset.json.
	UPROPERTY(EditAnywhere, Category = "Feu|Carte")
	int64 TerrainIonAssetId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Départ", meta = (ClampMin = "41.3", ClampMax = "43.1"))
	double IgnitionLatitude = 42.62;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Départ", meta = (ClampMin = "8.5", ClampMax = "9.6"))
	double IgnitionLongitude = 9.355;

	// Allume le feu au point de départ ci-dessus au lancement du jeu.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Départ")
	bool bIgniteOnBeginPlay = true;

	// Touche qui allume un feu sous la souris en jeu (au centre de la vue si la souris est cachée).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Départ")
	FKey IgniteKey = EKeys::F;

	// Vent moyen à 10 m.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Météo", meta = (ClampMin = "0", ClampMax = "150", Units = "km/h"))
	double WindSpeedKmh = 45;

	// Direction d'où vient le vent : 225 = libeccio (sud-ouest), 315 = mistral (nord-ouest).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Météo", meta = (ClampMin = "0", ClampMax = "360", Units = "deg"))
	double WindFromDegrees = 225;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Météo")
	EFireMoisture Moisture = EFireMoisture::Extreme;

	// Vent corrigé par le relief (atlas WindNinja), sinon vent uniforme.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Météo")
	bool bTerrainWind = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Météo", meta = (ClampMin = "0.5", ClampMax = "48", Units = "hrs"))
	double DurationHours = 6;

	// Météo réelle au point de départ (prévisions ou archives, téléchargées sur Open-Meteo) à la place du vent réglé :
	// vent heure par heure, humidité du bois mort calculée ; la sécheresse ne règle plus que la végétation vivante.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Météo")
	bool bRealWeather = false;

	// Heure locale du départ du feu, pour la météo réelle : de 1940 aux prévisions des 3 prochains jours.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Météo", meta = (EditCondition = "bRealWeather"))
	FDateTime IgnitionTime;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Feu|Météo")
	FString WeatherSource;

	// Heure du feu affichée sur le terrain.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Lecture", meta = (ClampMin = "0", UIMax = "48", Units = "hrs"))
	double DisplayedHours = 6;

	// Vitesse de lecture en jeu : heures de feu par seconde réelle.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Lecture", meta = (ClampMin = "0"))
	double PlaybackSpeed = 0.25;

	// Largeur du front actif, qui brille sur le terrain.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Lecture", meta = (ClampMin = "1", Units = "min"))
	double FrontMinutes = 20;

	UPROPERTY(EditAnywhere, Category = "Feu|Affichage")
	TSoftObjectPtr<UMaterialInterface> OverlayMaterial;

	// Panneau de réglage en jeu, avec la souris visible : la caméra tourne en glissant dans la vue.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Interface")
	bool bShowInterface = true;

	// Touche qui masque ou réaffiche le panneau, en jeu.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Interface")
	FKey ToggleInterfaceKey = EKeys::H;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Feu|Résultat")
	FString Status;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Feu|Résultat")
	double BurnedAreaHa = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Feu|Résultat", meta = (Units = "km/h"))
	double MaxSpreadKmh = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Feu|Résultat", meta = (Units = "m"))
	double MaxFlameLengthM = 0;

	// Simule un feu au point de départ (latitude, longitude) avec la météo réglée.
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Feu")
	void Simulate();

	UFUNCTION(BlueprintCallable, Category = "Feu")
	bool IgniteAtLongitudeLatitude(double Longitude, double Latitude);

	UFUNCTION(BlueprintCallable, Category = "Feu")
	bool IgniteAtWorldLocation(const FVector& Location);

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Feu")
	void ClearFire();

	// Rejoue un feu réel du catalogue : même point, même heure, météo réelle ; la zone réellement brûlée est
	// dessinée en bleu sur le terrain.
	UFUNCTION(BlueprintCallable, Category = "Feu")
	bool StartReplay(const FString& Id);

	// Recalcule le feu sans revenir au début : ce qui a déjà brûlé ne change pas, la lutte s'applique à partir de son heure.
	void Recalculate();

	UFireSuppressionComponent* GetSuppression() const { return Suppression; }

	// Prépare la carte : géoréférence centrée sur la Corse, terrain IGN depuis Cesium ion,
	// photo aérienne IGN, soleil, caméra au-dessus du point de départ.
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Feu")
	void PrepareScene();

	// Lecture du feu, pour le panneau en jeu.
	bool HasFire() const { return bHasFire; }
	bool IsPlaying() const { return bPlaying; }
	// Reprend depuis le départ si le feu est déjà affiché jusqu'au bout.
	void SetPlaying(bool bPlay);
	double GetSimulatedHours() const { return SimulatedMinutes / 60.0; }
	void SetDisplayedHours(double Hours);
	// Surface brûlée à cette heure du feu, en hectares.
	double AreaAt(double Hours) const;
	bool IsLoadingWeather() const { return bLoadingWeather; }
	// Météo réelle à cette heure du feu ; faux si le feu affiché a été calculé avec le vent réglé.
	bool RealWeatherAt(double FireHours, FFireWeatherNow& Out) const;
	const TArray<FFireReplay>& GetReplays() const { return Replays; }
	const FFireReplay* GetActiveReplay() const { return Replays.IsValidIndex(ActiveReplay) ? &Replays[ActiveReplay] : nullptr; }
	// Comparaison au feu réel rejoué, à cette heure du feu : surface simulée (ha) et indice de Sørensen
	// (1 si les deux zones coïncident, 0 si elles ne se touchent pas).
	bool ReplayScoresAt(double FireHours, double& SimulatedHa, double& Sorensen) const;

	virtual void Tick(float DeltaSeconds) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	friend class UFireSuppressionComponent;

	FString ResolvedDataDirectory() const;
	void StartAerialPhoto();
	void ShowFire(const TArray<FColor>& Pixels);
	UDecalComponent* MakeDecal(FName Name);
	void PlaceDecal(UDecalComponent* Target, const FFireDecalWindow& Window);
	void UpdateDisplayedTime();
	int32 BurnedCountAt(double Hours) const;
	// Catalogue des feux réels, lu dans data/processed/feux.
	void LoadReplays();
	void ShowObservedArea(const FFireReplay& Replay);
	// Pour chaque nombre de cellules brûlées (dans l'ordre d'arrivée), combien sont dans la zone réellement brûlée.
	void UpdateReplayOverlap();
	// La météo réelle chargée sert-elle pour ce point, l'heure de départ et la durée réglées ?
	bool RealWeatherCovers(double Longitude, double Latitude) const;
	// Télécharge la météo réelle, puis relance le calcul avec les derniers réglages.
	void LoadRealWeather();
	void CreateInterface(APlayerController& Controller);
	// Panneau affiché : souris visible, la caméra tourne en glissant. Masqué : vol libre à la souris.
	void ApplyInputMode(APlayerController& Controller) const;
	void IgniteUnderCursor(APlayerController& Controller);

	UFUNCTION()
	void HandleGeoreferenceUpdated();

	UPROPERTY(VisibleAnywhere, Category = "Feu")
	TObjectPtr<USceneComponent> Root;

	// Lutte contre le feu : ordres donnés en jeu, chantiers, avions.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Feu", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UFireSuppressionComponent> Suppression;

	bool bKeepDisplayedTime = false;

	UPROPERTY(Transient)
	TObjectPtr<UDecalComponent> Decal;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> FireTexture;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> OverlayInstance;

	// Contour de la zone réellement brûlée, pour le rejeu : même matériau, réglé pour n'afficher que le bord.
	UPROPERTY(Transient)
	TObjectPtr<UDecalComponent> ObservedDecal;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ObservedTexture;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ObservedInstance;

	// Panneau posé sur la vue du jeu (SFireSimPanel dans une boîte qui le place en haut à gauche).
	TSharedPtr<SWidget> Interface;

	// Dernier feu simulé : emprise, cellules brûlées (rang dans la grille) et leurs heures d'arrivée (minutes),
	// dans l'ordre d'arrivée.
	bool bHasFire = false;
	bool bPlaying = false;
	FFireDecalWindow FireWindow;
	double SimulatedMinutes = 0;
	TArray<float> ArrivalMinutes;
	TArray<int64> BurnedCells;

	// Feux réels : catalogue, feu rejoué, emprise de son contour et recouvrement avec le feu simulé.
	TArray<FFireReplay> Replays;
	int32 ActiveReplay = INDEX_NONE;
	FFireDecalWindow ObservedWindow;
	TArray<int32> ObservedPrefix;

	// Dernière météo réelle téléchargée ; celle du feu affiché, et le rang de son heure de départ.
	TSharedPtr<FRealWeather> RealWeather;
	TSharedPtr<FRealWeather> FireWeather;
	int32 FireWeatherStart = INDEX_NONE;
	FDateTime FireStartTime;
	bool bLoadingWeather = false;
};

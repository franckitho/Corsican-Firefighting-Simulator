#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include <optional>
#include <vector>

#include "firesim/suppression.hpp"

#include "FireSuppression.generated.h"

class AFireAircraft;
class AFireSimulator;
class APlayerController;
class SWidget;
class UDecalComponent;
class UGameViewportClient;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UTexture2D;

// Outil de lutte sélectionné en jeu.
UENUM()
enum class EFireTool : uint8
{
	None,
	Firebreak,
	Canadair,
	Dash,
};

// Lutte contre le feu, en jeu : les ordres donnés par le joueur, ce qu'ils deviennent (chantiers, vols) et ce qu'on en
// voit (tracés sur le terrain, avions, largages). Le calcul lui-même est dans sim/ (firesim/suppression.hpp).
UCLASS(BlueprintType)
class UFireSuppressionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFireSuppressionComponent();

	// --- Ordres, passés à l'heure affichée du feu ; chacun relance le calcul ---
	UFUNCTION(BlueprintCallable, Category = "Feu|Lutte")
	bool AddFirebreak(const TArray<FVector2D>& LatLon, double WidthM, int32 Engines);

	UFUNCTION(BlueprintCallable, Category = "Feu|Lutte")
	bool AddDrop(bool bDash, FVector2D FromLatLon, FVector2D ToLatLon);

	UFUNCTION(BlueprintCallable, Category = "Feu|Lutte")
	void ClearActions();

	void RemoveAction(int32 Index);

	// Caméra derrière cet avion ; un nom vide la libère.
	UFUNCTION(BlueprintCallable, Category = "Feu|Lutte")
	void FollowFlight(const FString& Callsign);

	// --- Outils du panneau ---
	EFireTool GetTool() const { return Tool; }
	void SetTool(EFireTool NewTool);
	void FinishDraft();
	void RemoveLastDraftPoint();
	int32 DraftPointCount() const { return Draft.Num(); }
	FString DraftText() const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Lutte", meta = (Units = "m"))
	double DraftWidthM = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Lutte")
	int32 DraftEngines = 2;

	// La lecture ralentit le temps de voir passer un largage.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feu|Lutte")
	bool bSlowDrops = true;

	// --- Affichage dans le panneau ---
	int32 GetRevision() const { return Revision; }  // change quand la liste des actions change
	int32 ActionCount() const { return Actions.Num(); }
	FString ActionText(int32 Index) const;
	FString FlightOfAction(int32 Index) const;  // indicatif de l'avion, vide pour un pare-feu
	const FString& GetFollowedFlight() const { return Followed; }
	FString ComparisonText() const;
	const FString& GetLastError() const { return LastError; }

	// --- Utilisé par le simulateur ---
	void Reset();
	// Prépare les ordres pour ce calcul : lève std::exception s'ils sont impossibles.
	std::vector<firesim::Treatment> Prepare(const firesim::Landscape& Land, const std::vector<float>& Elevation, double BreachFactor,
		const std::optional<firesim::Daylight>& Daylight);
	void SetResult(const firesim::SpreadResult& Fought, const firesim::SpreadResult* Free, double DurationMinutes);
	double FreeAreaAt(double Hours) const;
	void HandleInput(APlayerController& Controller);
	double AdjustSpeed(double PlaybackSpeed) const;
	void PlaceOverlays();
	void CreateInterface(UGameViewportClient& Viewport);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	struct FAction
	{
		bool bFirebreak = true;
		firesim::FirebreakOrder Firebreak;
		firesim::DropOrder Drop;
		int32 Index = 0;  // rang dans Firebreaks ou dans les ordres de largage, au dernier calcul
	};

	struct FFlightView
	{
		firesim::Flight Flight;
		TArray<double> Heights;  // altitude à chaque point de la trajectoire (m au-dessus du niveau de la mer)
	};

	AFireSimulator* Simulator() const;
	double NowMinutes() const;
	FString TimeText(double Minutes) const;
	void Recalculate();
	void AddDraftPoint(const firesim::Lambert93& Point);
	void AddPointUnderCursor(APlayerController& Controller);

	// Terrain et vols
	double TerrainAt(const firesim::Lambert93& P) const;
	FVector WorldAt(const firesim::Lambert93& P, double HeightM) const;
	TArray<double> ComputeHeights(const firesim::Flight& Flight) const;
	double HeightAt(const FFlightView& View, double Minutes) const;
	FVector FlightWorldAt(const FFlightView& View, double Minutes) const;
	void RebuildAircraft();
	void UpdateAircraft(float DeltaTime);
	void UpdateCamera();
	bool IsDropInSight(double Minutes) const;
	void UpdateSpray(double Minutes, const FVector& Eye);

	// Calques
	void RebuildOverlayWindow();
	void RefreshActionsOverlay();
	void BuildSavedOverlay(const firesim::SpreadResult& Fought, const firesim::SpreadResult& Free, double DurationMinutes);
	UMaterialInstanceDynamic* MakeOverlayInstance(float Opacity);

	EFireTool Tool = EFireTool::None;
	TArray<firesim::Lambert93> Draft;
	FString LastError;
	int32 Revision = 0;
	FString Followed;

	TArray<FAction> Actions;
	std::vector<firesim::Firebreak> Firebreaks;
	std::vector<firesim::Treatment> Treatments;
	std::vector<firesim::LineOutcome> Lines;
	std::vector<FFlightView> Flights;
	TArray<float> FreeArrival;  // heures d'arrivée du feu sans lutte, en ordre croissant

	const firesim::Landscape* Land = nullptr;
	const std::vector<float>* Elevation = nullptr;
	double SimulatedMinutes = 1;
	double BreachFactor = 1.5;
	double CellSize = 25;
	bool bHasDaylight = false;

	// Détection d'un clic, par opposition au glissement qui fait tourner la caméra
	double PressedAt = -1;
	float PressedDragPixels = 0;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AFireAircraft>> Aircraft;

	// Calque des actions : pare-feu, bandes de largage, tracé en cours. Pixels recalculés au fil du temps.
	int32 WindowCol = 0, WindowRow = 0, WindowColumns = 0, WindowRows = 0;
	double WindowMinElevation = 0, WindowMaxElevation = 0;
	TArray<FColor> ActionPixels;

	UPROPERTY(Transient)
	TObjectPtr<UDecalComponent> ActionsDecal;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ActionsTexture;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ActionsInstance;

	// Calque de ce que la lutte a sauvé : vert, là où le feu sans lutte serait déjà passé.
	int32 SavedColumns = 0, SavedRows = 0;
	double SavedWest = 0, SavedNorth = 0, SavedMinElevation = 0, SavedMaxElevation = 0;

	UPROPERTY(Transient)
	TObjectPtr<UDecalComponent> SavedDecal;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> SavedTexture;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SavedInstance;

	// Gouttes des largages : eau, retardant
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> WaterSpray;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> RetardantSpray;

	TSharedPtr<SWidget> Interface;
};

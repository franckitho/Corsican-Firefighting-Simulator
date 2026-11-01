#include "FireSuppression.h"

#include "CesiumGeoreference.h"
#include "CollisionQueryParams.h"
#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/PrimitiveComponent.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Widgets/Layout/SBox.h"

#include "FireAircraft.h"
#include "FireLuttePanel.h"
#include "FireSimulator.h"

#include <algorithm>
#include <cmath>
#include <exception>

using firesim::FlightPhase;

namespace
{
// Altitude du géoïde au-dessus de l'ellipsoïde en Corse (EGM96), comme le calque du feu.
constexpr double kGeoidHeightM = 49.0;
constexpr int32 kWindowMargin = 2;

// Altitudes de vol, en mètres.
constexpr double kMinCruiseM = 900.0, kCruiseClearanceM = 300.0, kApproachClearanceM = 120.0, kMinClearanceM = 60.0;
constexpr double kScoopHeightM = 2.0;
constexpr double kClearanceSampleM = 500.0;

// Taille à l'écran : l'avion n'a jamais moins de cette part de la distance à la caméra (40 pixels d'envergure environ).
constexpr double kMinApparentSize = 1.0 / 24.0;
constexpr double kWingspanM = 28.5;
constexpr double kMaxDisplayScale = 80.0;

// Lecture : réel x 5 pendant un largage en vue, x 15 derrière un avion, en heures de feu par seconde.
constexpr double kDropSpeed = 5.0 / 3600.0, kFollowSpeed = 15.0 / 3600.0;
constexpr double kDropInSightM = 40000.0;
constexpr double kBeforeDropMin = 30.0 / 60.0, kAfterDropMin = 20.0 / 60.0;

// Un largage : un rideau de traînées qui tombe derrière l'avion, fait de longues sphères serrées.
constexpr double kFallSpeedMS = 10.0;   // vitesse de chute
constexpr double kStreakSeconds = 1.5;  // durée d'écoulement en un point : longueur de la traînée
constexpr int32 kSprayParticles = 450;
constexpr double kSprayWidthM = 3.5;
constexpr double kMaxSprayScale = 12.0;

// Un clic : moins d'une demi-seconde, sans bouger la souris.
constexpr double kClickSeconds = 0.5;
constexpr float kClickDragPixels = 6.f;

const FColor kBuilt(255, 242, 190), kPlanned(240, 190, 50), kLost(120, 120, 120), kDraftColor(255, 140, 20);
const FColor kWater(70, 170, 255), kRetardant(225, 25, 25), kPlannedWater(150, 200, 240), kPlannedRetardant(240, 130, 130);
const FColor kSavedColor(70, 225, 110);

constexpr double kFollowBackM = 95.0, kFollowUpM = 28.0;

FString Thousands(double Value)
{
	FString Digits = FString::Printf(TEXT("%.0f"), Value);
	for (int32 I = Digits.Len() - 3; I > 0; I -= 3)
	{
		Digits.InsertAt(I, TEXT(" "));
	}
	return Digits;
}

FString OneDecimal(double Value)
{
	return FString::Printf(TEXT("%.1f"), Value).Replace(TEXT("."), TEXT(","));
}

bool IsFlying(FlightPhase Phase)
{
	return Phase != FlightPhase::Parked && Phase != FlightPhase::Reload;
}

// Valeur d'apparence aléatoire mais toujours la même pour un rang donné, pour que les gouttes ne scintillent pas.
double Noise(int32 Seed)
{
	const double Value = FMath::Sin(Seed * 12.9898) * 43758.5453;
	return Value - FMath::FloorToDouble(Value);
}

UTexture2D* MakeTexture(int32 Columns, int32 Rows, const TArray<FColor>& Pixels)
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
}  // namespace

UFireSuppressionComponent::UFireSuppressionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

AFireSimulator* UFireSuppressionComponent::Simulator() const
{
	return Cast<AFireSimulator>(GetOwner());
}

double UFireSuppressionComponent::NowMinutes() const
{
	const AFireSimulator* Sim = Simulator();
	return Sim ? Sim->DisplayedHours * 60.0 : 0.0;
}

FString UFireSuppressionComponent::TimeText(double Minutes) const
{
	if (!std::isfinite(Minutes))
	{
		return TEXT("?");
	}
	const AFireSimulator* Sim = Simulator();
	if (bHasDaylight && Sim)
	{
		return (Sim->IgnitionTime + FTimespan::FromMinutes(Minutes)).ToString(TEXT("%H h %M"));
	}
	const int32 Total = FMath::RoundToInt(Minutes);
	return FString::Printf(TEXT("+%d h %02d"), Total / 60, Total % 60);
}

void UFireSuppressionComponent::Recalculate()
{
	++Revision;
	if (AFireSimulator* Sim = Simulator())
	{
		Sim->Recalculate();
	}
}

bool UFireSuppressionComponent::AddFirebreak(const TArray<FVector2D>& LatLon, double WidthM, int32 Engines)
{
	if (LatLon.Num() < 2)
	{
		LastError = TEXT("Un pare-feu a besoin d'au moins deux points.");
		return false;
	}
	FAction Action;
	Action.bFirebreak = true;
	Action.Firebreak.order_min = NowMinutes();
	Action.Firebreak.width_m = WidthM;
	Action.Firebreak.engines = Engines;
	for (const FVector2D& Point : LatLon)
	{
		Action.Firebreak.path.push_back(firesim::to_lambert93({Point.X, Point.Y}));
	}
	if (Land)
	{
		try
		{
			firesim::plan_firebreak(*Land, Action.Firebreak, BreachFactor);
		}
		catch (const std::exception& Error)
		{
			LastError = UTF8_TO_TCHAR(Error.what());
			return false;
		}
	}
	LastError.Reset();
	Actions.Add(MoveTemp(Action));
	Recalculate();
	return true;
}

bool UFireSuppressionComponent::AddDrop(bool bDash, FVector2D FromLatLon, FVector2D ToLatLon)
{
	FAction Action;
	Action.bFirebreak = false;
	Action.Drop.kind = bDash ? firesim::AircraftKind::Dash : firesim::AircraftKind::Canadair;
	Action.Drop.order_min = NowMinutes();
	Action.Drop.from = firesim::to_lambert93({FromLatLon.X, FromLatLon.Y});
	Action.Drop.to = firesim::to_lambert93({ToLatLon.X, ToLatLon.Y});
	LastError.Reset();
	Actions.Add(MoveTemp(Action));
	Recalculate();
	return true;
}

void UFireSuppressionComponent::ClearActions()
{
	Actions.Reset();
	Followed.Reset();
	Recalculate();
}

void UFireSuppressionComponent::RemoveAction(int32 Index)
{
	if (Actions.IsValidIndex(Index))
	{
		Actions.RemoveAt(Index);
		Recalculate();
	}
}

void UFireSuppressionComponent::Reset()
{
	Actions.Reset();
	Draft.Reset();
	Firebreaks.clear();
	Treatments.clear();
	Lines.clear();
	Flights.clear();
	FreeArrival.Reset();
	LastError.Reset();
	Followed.Reset();
	Tool = EFireTool::None;
	++Revision;
	RebuildAircraft();
	RebuildOverlayWindow();
	if (SavedDecal)
	{
		SavedDecal->SetVisibility(false);
	}
}

void UFireSuppressionComponent::FollowFlight(const FString& Callsign)
{
	Followed = Callsign;
}

void UFireSuppressionComponent::SetTool(EFireTool NewTool)
{
	Tool = NewTool == Tool ? EFireTool::None : NewTool;
	Draft.Reset();
	LastError.Reset();
	if (Tool != EFireTool::None)
	{
		if (AFireSimulator* Sim = Simulator())
		{
			Sim->SetPlaying(false);  // on place ses actions feu arrêté
		}
	}
	RebuildOverlayWindow();
}

void UFireSuppressionComponent::AddDraftPoint(const firesim::Lambert93& Point)
{
	Draft.Add(Point);
	if ((Tool == EFireTool::Canadair || Tool == EFireTool::Dash) && Draft.Num() == 2)
	{
		FAction Action;
		Action.bFirebreak = false;
		Action.Drop.kind = Tool == EFireTool::Dash ? firesim::AircraftKind::Dash : firesim::AircraftKind::Canadair;
		Action.Drop.order_min = NowMinutes();
		Action.Drop.from = Draft[0];
		Action.Drop.to = Draft[1];
		Draft.Reset();
		Actions.Add(MoveTemp(Action));
		Recalculate();
		return;
	}
	RebuildOverlayWindow();
}

void UFireSuppressionComponent::RemoveLastDraftPoint()
{
	if (!Draft.IsEmpty())
	{
		Draft.Pop();
		RebuildOverlayWindow();
	}
}

void UFireSuppressionComponent::FinishDraft()
{
	if (Tool != EFireTool::Firebreak || Draft.Num() < 2)
	{
		return;
	}
	FAction Action;
	Action.bFirebreak = true;
	Action.Firebreak.order_min = NowMinutes();
	Action.Firebreak.width_m = DraftWidthM;
	Action.Firebreak.engines = DraftEngines;
	Action.Firebreak.path.assign(Draft.GetData(), Draft.GetData() + Draft.Num());
	if (Land)
	{
		try
		{
			firesim::plan_firebreak(*Land, Action.Firebreak, BreachFactor);
		}
		catch (const std::exception& Error)
		{
			LastError = UTF8_TO_TCHAR(Error.what());
			return;
		}
	}
	LastError.Reset();
	Draft.Reset();
	Actions.Add(MoveTemp(Action));
	Recalculate();
}

FString UFireSuppressionComponent::DraftText() const
{
	if (Tool == EFireTool::None)
	{
		return FString();
	}
	if (Tool != EFireTool::Firebreak)
	{
		const firesim::AircraftType& Type = firesim::aircraft_type(Tool == EFireTool::Dash ? firesim::AircraftKind::Dash : firesim::AircraftKind::Canadair);
		return FString::Printf(TEXT("%s (%s) : cliquez le début, puis la fin de l'axe. Un passage couvre %.0f m ; départ de sa base à l'heure affichée."),
			UTF8_TO_TCHAR(Type.name), UTF8_TO_TCHAR(Type.fluid), Type.strip_m);
	}
	const double HoldsM = DraftWidthM / BreachFactor;
	FString Text = FString::Printf(TEXT("Cliquez le tracé sur le terrain, dans le sens du chantier. Tient jusqu'à %s m de flammes."), *OneDecimal(HoldsM));
	if (Land && Draft.Num() >= 2)
	{
		firesim::FirebreakOrder Order;
		Order.order_min = NowMinutes();
		Order.width_m = DraftWidthM;
		Order.engines = DraftEngines;
		Order.path.assign(Draft.GetData(), Draft.GetData() + Draft.Num());
		try
		{
			const firesim::Firebreak Line = firesim::plan_firebreak(*Land, Order, BreachFactor);
			Text = FString::Printf(TEXT("%s km, tient jusqu'à %s m de flammes.\nEngins sur place à %s, ligne complète à %s."),
				*OneDecimal(Line.length_m / 1000.0), *OneDecimal(HoldsM), *TimeText(Line.start_min), *TimeText(Line.finish_min));
		}
		catch (const std::exception& Error)
		{
			Text = UTF8_TO_TCHAR(Error.what());
		}
	}
	return Text;
}

FString UFireSuppressionComponent::FlightOfAction(int32 Index) const
{
	if (!Actions.IsValidIndex(Index) || Actions[Index].bFirebreak)
	{
		return FString();
	}
	for (const FFlightView& View : Flights)
	{
		for (const firesim::AirDrop& Drop : View.Flight.drops)
		{
			if (Drop.order == Actions[Index].Index)
			{
				return UTF8_TO_TCHAR(View.Flight.callsign.c_str());
			}
		}
	}
	return FString();
}

FString UFireSuppressionComponent::ActionText(int32 Index) const
{
	if (!Actions.IsValidIndex(Index))
	{
		return FString();
	}
	const FAction& Action = Actions[Index];
	const double Now = NowMinutes();
	if (Action.bFirebreak)
	{
		if (Action.Index < 0 || Action.Index >= static_cast<int32>(Firebreaks.size()))
		{
			return TEXT("Pare-feu");
		}
		const firesim::Firebreak& Line = Firebreaks[Action.Index];
		FString Text = FString::Printf(TEXT("Pare-feu : %s km, %.0f m, %d engin%s ; tient jusqu'à %s m de flammes\n"),
			*OneDecimal(Line.length_m / 1000.0), Action.Firebreak.width_m, Action.Firebreak.engines, Action.Firebreak.engines > 1 ? TEXT("s") : TEXT(""),
			*OneDecimal(Line.holds_flame_m));
		const firesim::LineOutcome Outcome = Action.Index < static_cast<int32>(Lines.size()) ? Lines[Action.Index] : firesim::LineOutcome{};
		const double Stop = std::min<double>(Outcome.cut_from, Line.finish_min);
		const auto Done = std::count_if(Line.built_min.begin(), Line.built_min.end(), [&](float Built) { return Built <= FMath::Min(Now, Stop); });
		const int32 Percent = FMath::RoundToInt(100.0 * Done / FMath::Max<size_t>(1, Line.built_min.size()));
		if (Now < Line.start_min)
		{
			Text += FString::Printf(TEXT("Engins en route, sur place à %s ; ligne complète à %s"), *TimeText(Line.start_min), *TimeText(Line.finish_min));
		}
		else if (std::isfinite(Outcome.cut_min) && Now >= Outcome.cut_min)
		{
			Text += FString::Printf(TEXT("Chantier atteint par le feu à %s : %d %% construit"), *TimeText(Outcome.cut_min), Percent);
		}
		else if (Now < Line.finish_min)
		{
			Text += FString::Printf(TEXT("Chantier : %d %%, ligne complète à %s"), Percent, *TimeText(Line.finish_min));
		}
		else
		{
			Text += FString::Printf(TEXT("Ligne opérationnelle depuis %s"), *TimeText(Line.finish_min));
		}
		if (std::isfinite(Outcome.breach_min) && Now >= Outcome.breach_min)
		{
			Text += FString::Printf(TEXT(" ; franchie à %s"), *TimeText(Outcome.breach_min));
		}
		return Text;
	}

	const firesim::AircraftType& Type = firesim::aircraft_type(Action.Drop.kind);
	const int32 Passes = firesim::drop_passes(Action.Drop);
	for (const FFlightView& View : Flights)
	{
		const firesim::AirDrop* First = nullptr;
		const firesim::AirDrop* Last = nullptr;
		int32 Done = 0;
		for (const firesim::AirDrop& Drop : View.Flight.drops)
		{
			if (Drop.order != Action.Index)
			{
				continue;
			}
			First = First ? First : &Drop;
			Last = &Drop;
			Done += Drop.start_min <= Now ? 1 : 0;
		}
		if (!First)
		{
			continue;
		}
		FString State;
		switch (View.Flight.at(Now).phase)
		{
		case FlightPhase::Parked: State = Now < First->start_min ? TEXT("au sol") : TEXT("posé"); break;
		case FlightPhase::Reload: State = TEXT("au remplissage"); break;
		case FlightPhase::Climb: State = TEXT("décolle"); break;
		case FlightPhase::Scoop: State = TEXT("écope"); break;
		case FlightPhase::Approach: State = TEXT("en approche"); break;
		case FlightPhase::Drop: State = TEXT("largue"); break;
		case FlightPhase::Descent: State = TEXT("atterrit"); break;
		default: State = TEXT("en vol"); break;
		}
		return FString::Printf(TEXT("%s (%s) depuis %s, %s\nPremier largage à %s, %d/%d passage%s, fin à %s"),
			UTF8_TO_TCHAR(View.Flight.callsign.c_str()), UTF8_TO_TCHAR(Type.fluid), UTF8_TO_TCHAR(View.Flight.origin.c_str()), *State,
			*TimeText(First->start_min), Done, Passes, Passes > 1 ? TEXT("s") : TEXT(""), *TimeText(Last->end_min));
	}
	return FString::Printf(TEXT("%s (%s)"), UTF8_TO_TCHAR(Type.name), UTF8_TO_TCHAR(Type.fluid));
}

FString UFireSuppressionComponent::ComparisonText() const
{
	const AFireSimulator* Sim = Simulator();
	if (!Sim || FreeArrival.IsEmpty())
	{
		return FString();
	}
	const double Fought = Sim->AreaAt(Sim->DisplayedHours);
	const double Free = FreeAreaAt(Sim->DisplayedHours);
	return FString::Printf(TEXT("À cette heure : %s ha avec la lutte, %s ha sans.\nÉvités : %s ha (en vert sur le terrain)."),
		*Thousands(Fought), *Thousands(Free), *Thousands(FMath::Max(0.0, Free - Fought)));
}

double UFireSuppressionComponent::FreeAreaAt(double Hours) const
{
	const float Minutes = static_cast<float>(Hours * 60.0);
	const int64 Count = std::upper_bound(FreeArrival.GetData(), FreeArrival.GetData() + FreeArrival.Num(), Minutes) - FreeArrival.GetData();
	return Count * CellSize * CellSize / 10000.0;
}

std::vector<firesim::Treatment> UFireSuppressionComponent::Prepare(const firesim::Landscape& InLand, const std::vector<float>& InElevation,
	double InBreachFactor, const std::optional<firesim::Daylight>& Daylight)
{
	Land = &InLand;
	Elevation = &InElevation;
	BreachFactor = InBreachFactor;
	CellSize = InLand.grid.res;
	bHasDaylight = Daylight.has_value();

	Firebreaks.clear();
	std::vector<firesim::DropOrder> DropOrders;
	for (FAction& Action : Actions)
	{
		if (Action.bFirebreak)
		{
			Action.Index = static_cast<int32>(Firebreaks.size());
			Firebreaks.push_back(firesim::plan_firebreak(InLand, Action.Firebreak, BreachFactor));
		}
		else
		{
			Action.Index = static_cast<int32>(DropOrders.size());
			DropOrders.push_back(Action.Drop);
		}
	}
	const std::vector<firesim::Flight> Planned = firesim::plan_air_attack(InLand, DropOrders, Daylight);
	Flights.clear();
	for (const firesim::Flight& Flight : Planned)
	{
		FFlightView View;
		View.Flight = Flight;
		View.Heights = ComputeHeights(Flight);
		Flights.push_back(MoveTemp(View));
	}
	Treatments = firesim::treatments_of(InLand.grid, Firebreaks, Planned);
	return Treatments;
}

void UFireSuppressionComponent::SetResult(const firesim::SpreadResult& Fought, const firesim::SpreadResult* Free, double DurationMinutes)
{
	SimulatedMinutes = DurationMinutes;
	Lines = Fought.lines;
	FreeArrival.Reset();
	if (Free)
	{
		FreeArrival.Reserve(static_cast<int32>(Free->cells.size()));
		for (const firesim::BurnedCell& Cell : Free->cells)
		{
			FreeArrival.Add(Cell.arrival_min);
		}
		BuildSavedOverlay(Fought, *Free, DurationMinutes);
	}
	else if (SavedDecal)
	{
		SavedDecal->SetVisibility(false);
	}
	++Revision;
	RebuildAircraft();
	RebuildOverlayWindow();
}

double UFireSuppressionComponent::AdjustSpeed(double PlaybackSpeed) const
{
	double Speed = PlaybackSpeed;
	if (!Followed.IsEmpty())
	{
		Speed = FMath::Min(Speed, kFollowSpeed);
	}
	if (bSlowDrops && IsDropInSight(NowMinutes()))
	{
		Speed = FMath::Min(Speed, kDropSpeed);
	}
	return Speed;
}

bool UFireSuppressionComponent::IsDropInSight(double Minutes) const
{
	const UWorld* World = GetWorld();
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	if (!Controller || Flights.empty())
	{
		return false;
	}
	FVector Eye;
	FRotator View;
	Controller->GetPlayerViewPoint(Eye, View);
	for (const FFlightView& Flight : Flights)
	{
		for (const firesim::AirDrop& Drop : Flight.Flight.drops)
		{
			if (Minutes >= Drop.start_min - kBeforeDropMin && Minutes <= Drop.end_min + kAfterDropMin &&
				FVector::Dist(WorldAt(Drop.from, TerrainAt(Drop.from)), Eye) / 100.0 < kDropInSightM)
			{
				return true;
			}
		}
	}
	return false;
}

void UFireSuppressionComponent::HandleInput(APlayerController& Controller)
{
	if (Tool == EFireTool::None)
	{
		PressedAt = -1;
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Controller.WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		PressedAt = Now;
		PressedDragPixels = 0;
	}
	if (PressedAt >= 0 && Controller.IsInputKeyDown(EKeys::LeftMouseButton))
	{
		float DeltaX = 0, DeltaY = 0;
		Controller.GetInputMouseDelta(DeltaX, DeltaY);
		PressedDragPixels += FMath::Abs(DeltaX) + FMath::Abs(DeltaY);
	}
	if (Controller.WasInputKeyJustReleased(EKeys::LeftMouseButton))
	{
		if (PressedAt >= 0 && Now - PressedAt < kClickSeconds && PressedDragPixels < kClickDragPixels)
		{
			AddPointUnderCursor(Controller);
		}
		PressedAt = -1;
	}
	if (Tool == EFireTool::Firebreak && (Controller.WasInputKeyJustPressed(EKeys::RightMouseButton) || Controller.WasInputKeyJustPressed(EKeys::Enter)))
	{
		FinishDraft();
	}
	if (Controller.WasInputKeyJustPressed(EKeys::BackSpace))
	{
		RemoveLastDraftPoint();
	}
}

void UFireSuppressionComponent::AddPointUnderCursor(APlayerController& Controller)
{
	FVector Eye, Direction;
	if (!Controller.DeprojectMousePositionToWorld(Eye, Direction))
	{
		return;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FireSimLutte), true, Controller.GetPawn());
	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Direction * 5.0e7, ECC_Visibility, Params))
	{
		return;
	}
	ACesiumGeoreference* Georeference = ACesiumGeoreference::GetDefaultGeoreferenceForActor(GetOwner());
	const FVector Local = Georeference->GetActorTransform().InverseTransformPosition(Hit.ImpactPoint);
	const FVector LonLatHeight = Georeference->TransformUnrealPositionToLongitudeLatitudeHeight(Local);
	AddDraftPoint(firesim::to_lambert93({LonLatHeight.Y, LonLatHeight.X}));
}

double UFireSuppressionComponent::TerrainAt(const firesim::Lambert93& P) const
{
	if (!Land || !Elevation)
	{
		return 0.0;
	}
	const auto Cell = Land->grid.cell_at(P.x, P.y);
	if (!Cell)
	{
		return 0.0;
	}
	const float Height = (*Elevation)[Land->grid.index(Cell->col, Cell->row)];
	return FMath::IsFinite(Height) ? FMath::Max(0.f, Height) : 0.0;
}

FVector UFireSuppressionComponent::WorldAt(const firesim::Lambert93& P, double HeightM) const
{
	ACesiumGeoreference* Georeference = ACesiumGeoreference::GetDefaultGeoreferenceForActor(GetOwner());
	const firesim::LatLon Point = firesim::from_lambert93(P);
	return Georeference->GetActorTransform().TransformPosition(Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(
		FVector(Point.lon, Point.lat, HeightM + kGeoidHeightM)));
}

TArray<double> UFireSuppressionComponent::ComputeHeights(const firesim::Flight& Flight) const
{
	const firesim::AircraftType& Type = firesim::aircraft_type(Flight.kind);
	const std::vector<firesim::Waypoint>& Track = Flight.track;
	auto CruiseHeight = [&](const firesim::Lambert93& A, const firesim::Lambert93& B)
	{
		const double Length = std::hypot(B.x - A.x, B.y - A.y);
		const int32 Samples = FMath::CeilToInt32(Length / kClearanceSampleM);
		double Highest = 0.0;
		for (int32 I = 0; I <= Samples; ++I)
		{
			const double F = Samples > 0 ? static_cast<double>(I) / Samples : 0.0;
			Highest = FMath::Max(Highest, TerrainAt({A.x + (B.x - A.x) * F, A.y + (B.y - A.y) * F}));
		}
		return FMath::Max(kMinCruiseM, Highest + kCruiseClearanceM);
	};

	TArray<double> Heights;
	Heights.SetNumZeroed(static_cast<int32>(Track.size()));
	for (int32 I = 0; I < Heights.Num(); ++I)
	{
		const firesim::Waypoint& Point = Track[I];
		const FlightPhase Before = I > 0 ? Track[I - 1].phase : FlightPhase::Parked;
		switch (Point.phase)
		{
		case FlightPhase::Parked:
		case FlightPhase::Reload:
		case FlightPhase::Climb:
			Heights[I] = TerrainAt(Point.position);
			break;
		case FlightPhase::Scoop:
			Heights[I] = kScoopHeightM;
			break;
		case FlightPhase::Approach:
			Heights[I] = TerrainAt(Point.position) + kApproachClearanceM;
			break;
		case FlightPhase::Drop:
		case FlightPhase::Exit:
			Heights[I] = TerrainAt(Point.position) + Type.drop_height_m;
			break;
		case FlightPhase::Cruise:
		case FlightPhase::Descent:
		default:
			if (Before == FlightPhase::Scoop)
			{
				Heights[I] = kScoopHeightM;
			}
			else if (Before == FlightPhase::Exit)
			{
				Heights[I] = TerrainAt(Point.position) + kApproachClearanceM;
			}
			else
			{
				const firesim::Lambert93& Next = I + 1 < Heights.Num() ? Track[I + 1].position : Point.position;
				Heights[I] = CruiseHeight(Point.position, Next);
			}
			break;
		}
	}
	return Heights;
}

double UFireSuppressionComponent::HeightAt(const FFlightView& View, double Minutes) const
{
	const std::vector<firesim::Waypoint>& Track = View.Flight.track;
	const firesim::Flight::State State = View.Flight.at(Minutes);
	const double Ground = TerrainAt(State.position);
	if (Minutes <= Track.front().time_min)
	{
		return View.Heights[0];
	}
	if (Minutes >= Track.back().time_min)
	{
		return View.Heights.Last();
	}
	int32 I = 0;
	while (I + 2 < static_cast<int32>(Track.size()) && Minutes >= Track[I + 1].time_min)
	{
		++I;
	}
	const double Span = Track[I + 1].time_min - Track[I].time_min;
	const double F = Span > 0 ? (Minutes - Track[I].time_min) / Span : 0.0;
	double Height = FMath::Lerp(View.Heights[I], View.Heights[I + 1], FMath::SmoothStep(0.0, 1.0, F));
	switch (State.phase)
	{
	case FlightPhase::Parked:
	case FlightPhase::Reload:
		return Ground;
	case FlightPhase::Scoop:
		return kScoopHeightM;
	case FlightPhase::Drop:
		return Ground + firesim::aircraft_type(View.Flight.kind).drop_height_m;
	case FlightPhase::Climb:
	case FlightPhase::Descent:
		return FMath::Max(Height, Ground);
	default:
		break;
	}
	// Ne pas entrer dans le relief ; la descente vers l'écopage se termine au ras de l'eau
	const bool bToWater = I + 1 < static_cast<int32>(Track.size()) && Track[I + 1].phase == FlightPhase::Scoop;
	return bToWater && Ground <= 0.0 ? Height : FMath::Max(Height, Ground + kMinClearanceM);
}

FVector UFireSuppressionComponent::FlightWorldAt(const FFlightView& View, double Minutes) const
{
	return WorldAt(View.Flight.at(Minutes).position, HeightAt(View, Minutes));
}

void UFireSuppressionComponent::RebuildAircraft()
{
	for (AFireAircraft* Plane : Aircraft)
	{
		if (Plane)
		{
			Plane->Destroy();
		}
	}
	Aircraft.Reset();
	UWorld* World = GetWorld();
	if (Flights.empty() || !World || !World->IsGameWorld())
	{
		return;
	}
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/FireSim/M_FireAircraft.M_FireAircraft"));
	if (!Material)
	{
		LastError = TEXT("Matériau des avions introuvable : lancer Scripts/create_assets.py");
	}
	for (const FFlightView& View : Flights)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AFireAircraft* Plane = World->SpawnActor<AFireAircraft>(AFireAircraft::StaticClass(), FTransform::Identity, Params);
		Plane->Build(View.Flight.kind == firesim::AircraftKind::Dash, Material);
		Aircraft.Add(Plane);
	}
}

void UFireSuppressionComponent::UpdateAircraft(float DeltaTime)
{
	const double Now = NowMinutes();
	const UWorld* World = GetWorld();
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	FVector Eye = FVector::ZeroVector;
	if (Controller)
	{
		FRotator View;
		Controller->GetPlayerViewPoint(Eye, View);
	}

	constexpr double kSmoothMin = 3.0 / 60.0, kTurnMin = 6.0 / 60.0;
	for (int32 I = 0; I < Aircraft.Num() && I < static_cast<int32>(Flights.size()); ++I)
	{
		AFireAircraft* Plane = Aircraft[I];
		const FFlightView& View = Flights[I];
		const bool bVisible = Now >= View.Flight.first_order_min;
		Plane->SetActorHiddenInGame(!bVisible);
		if (!bVisible)
		{
			continue;
		}
		const firesim::Flight::State State = View.Flight.at(Now);
		const bool bFlying = IsFlying(State.phase);

		// Trois points moyennés : les virages sont arrondis, sans changer l'heure de passage sur la bande
		const FVector Before = FlightWorldAt(View, Now - kSmoothMin), Here = FlightWorldAt(View, Now), After = FlightWorldAt(View, Now + kSmoothMin);
		FVector Position = (Before + Here + After) / 3.0;
		const FVector Up = (WorldAt(State.position, 1000.0) - WorldAt(State.position, 0.0)).GetSafeNormal();

		// Cap : la trajectoire, ou au sol, le dernier tronçon parcouru (le suivant s'il n'a pas encore volé)
		FVector Forward = After - Before;
		if (Forward.SizeSquared() < 1.0)
		{
			const std::vector<firesim::Waypoint>& Track = View.Flight.track;
			for (int32 K = static_cast<int32>(Track.size()) - 1; K > 0 && Forward.SizeSquared() < 1.0; --K)
			{
				if (Track[K].time_min <= Now && std::hypot(Track[K].position.x - Track[K - 1].position.x, Track[K].position.y - Track[K - 1].position.y) > 1.0)
				{
					Forward = WorldAt(Track[K].position, 0.0) - WorldAt(Track[K - 1].position, 0.0);
				}
			}
			for (size_t K = 0; K + 1 < Track.size() && Forward.SizeSquared() < 1.0; ++K)
			{
				if (std::hypot(Track[K + 1].position.x - Track[K].position.x, Track[K + 1].position.y - Track[K].position.y) > 1.0)
				{
					Forward = WorldAt(Track[K + 1].position, 0.0) - WorldAt(Track[K].position, 0.0);
				}
			}
		}
		Forward = Forward.GetSafeNormal();

		// Inclinaison dans les virages
		const FVector Earlier = (Here - FlightWorldAt(View, Now - kTurnMin)).GetSafeNormal();
		const FVector Later = (FlightWorldAt(View, Now + kTurnMin) - Here).GetSafeNormal();
		const double Turn = FVector::DotProduct(FVector::CrossProduct(Earlier, Later), Up);
		const double Bank = bFlying ? FMath::Clamp(Turn * 1.2, -0.6, 0.6) : 0.0;

		const bool bFollowed = !Followed.IsEmpty() && Followed == UTF8_TO_TCHAR(View.Flight.callsign.c_str());
		const double Distance = FVector::Dist(Eye, Position) / 100.0;
		const double DisplayScale = bFollowed ? 1.0 : FMath::Clamp(Distance * kMinApparentSize / kWingspanM, 1.0, kMaxDisplayScale);
		// Plus gros que nature : il reste au-dessus du sol
		Position += Up * (DisplayScale - 1.0) * (bFlying ? 500.0 : 250.0);

		const FQuat Level = FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat();
		Plane->SetActorLocationAndRotation(Position, FQuat(Forward, Bank) * Level);
		Plane->SetActorScale3D(FVector(DisplayScale));
		Plane->SpinPropellers(DeltaTime, bFlying);
		// Plus gros que nature, son ombre serait fausse et encombrerait la carte d'ombres
		const bool bShadow = DisplayScale < 1.5;
		Plane->ForEachComponent<UPrimitiveComponent>(false, [bShadow](UPrimitiveComponent* Part)
		{
			if (Part->CastShadow != bShadow)
			{
				Part->SetCastShadow(bShadow);
			}
		});
	}
	UpdateSpray(Now, Eye);
}

void UFireSuppressionComponent::UpdateSpray(double Minutes, const FVector& Eye)
{
	if (!GetWorld() || !GetWorld()->IsGameWorld())
	{
		return;
	}
	auto MakeSpray = [this](const FLinearColor& Color, float Opacity, FName Name)
	{
		UInstancedStaticMeshComponent* Spray = NewObject<UInstancedStaticMeshComponent>(GetOwner(), Name, RF_Transient);
		Spray->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		Spray->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Spray->SetCastShadow(false);
		Spray->SetReceivesDecals(false);  // les calques du terrain ne doivent pas se projeter sur les gouttes
		Spray->SetupAttachment(GetOwner()->GetRootComponent());
		if (UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/FireSim/M_FireDrop.M_FireDrop")))
		{
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Spray);
			Instance->SetVectorParameterValue(TEXT("Color"), Color);
			Instance->SetScalarParameterValue(TEXT("Opacity"), Opacity);
			Spray->SetMaterial(0, Instance);
		}
		Spray->RegisterComponent();
		return Spray;
	};
	if (!WaterSpray)
	{
		WaterSpray = MakeSpray(FLinearColor(0.85f, 0.93f, 1.0f), 0.28f, TEXT("GouttesEau"));
	}
	if (!RetardantSpray)
	{
		RetardantSpray = MakeSpray(FLinearColor(0.85f, 0.03f, 0.02f), 0.35f, TEXT("GouttesRetardant"));
	}
	WaterSpray->ClearInstances();
	RetardantSpray->ClearInstances();

	auto AddCloud = [&](UInstancedStaticMeshComponent* Spray, const firesim::Lambert93& From, const firesim::Lambert93& To, double Start, double End,
						double DropHeight, double SpreadM, int32 Seed)
	{
		const double LifeS = DropHeight / kFallSpeedMS + kStreakSeconds;
		if (Minutes < Start || Minutes > End + LifeS / 60.0)
		{
			return;
		}
		// Vu de loin, le rideau reste visible, comme l'avion
		const double Distance = FVector::Dist(Eye, WorldAt(From, TerrainAt(From))) / 100.0;
		const double Width = kSprayWidthM * FMath::Clamp(Distance * kMinApparentSize / kWingspanM, 1.0, kMaxSprayScale);
		for (int32 I = 0; I < kSprayParticles; ++I)
		{
			const double Along = static_cast<double>(I) / (kSprayParticles - 1);
			const double Born = Start + (End - Start) * Along;
			const double AgeS = (Minutes - Born) * 60.0;
			if (AgeS < 0.0 || AgeS > LifeS)
			{
				continue;
			}
			const firesim::Lambert93 Point{From.x + (To.x - From.x) * Along, From.y + (To.y - From.y) * Along};
			const firesim::Lambert93 Spot{Point.x + (Noise(Seed + I) - 0.5) * SpreadM, Point.y + (Noise(Seed + I + 500) - 0.5) * SpreadM};
			// Haut et bas de la traînée, au-dessus du sol : elle s'allonge sous l'avion, puis tombe
			const double Speed = kFallSpeedMS * (0.8 + 0.4 * Noise(Seed + I + 900));
			const double Bottom = FMath::Max(0.0, DropHeight - Speed * AgeS);
			const double Top = FMath::Max(Bottom + 1.0, DropHeight - Speed * FMath::Max(0.0, AgeS - kStreakSeconds));
			Spray->AddInstance(FTransform(FQuat::Identity, WorldAt(Spot, TerrainAt(Spot) + (Top + Bottom) / 2.0), FVector(Width, Width, Top - Bottom)), true);
		}
	};

	int32 Seed = 1;
	for (const FFlightView& View : Flights)
	{
		const bool bWater = View.Flight.kind == firesim::AircraftKind::Canadair;
		const firesim::AircraftType& Type = firesim::aircraft_type(View.Flight.kind);
		for (const firesim::AirDrop& Drop : View.Flight.drops)
		{
			AddCloud(bWater ? WaterSpray : RetardantSpray, Drop.from, Drop.to, Drop.start_min, Drop.end_min, Type.drop_height_m, 12.0, Seed += 1000);
		}
		// Écopage : gerbe blanche derrière l'avion
		for (size_t I = 0; I + 1 < View.Flight.track.size(); ++I)
		{
			const firesim::Waypoint& Point = View.Flight.track[I];
			if (Point.phase == FlightPhase::Scoop)
			{
				AddCloud(WaterSpray, Point.position, View.Flight.track[I + 1].position, Point.time_min, View.Flight.track[I + 1].time_min, 9.0, 8.0, Seed += 1000);
			}
		}
	}
}

void UFireSuppressionComponent::UpdateCamera()
{
	if (Followed.IsEmpty())
	{
		return;
	}
	UWorld* World = GetWorld();
	APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	for (int32 I = 0; I < Aircraft.Num() && I < static_cast<int32>(Flights.size()); ++I)
	{
		if (Followed != UTF8_TO_TCHAR(Flights[I].Flight.callsign.c_str()) || !Aircraft[I])
		{
			continue;
		}
		const FTransform Pose = Aircraft[I]->GetActorTransform();
		const FVector Up = (WorldAt(Flights[I].Flight.at(NowMinutes()).position, 1000.0) - WorldAt(Flights[I].Flight.at(NowMinutes()).position, 0.0)).GetSafeNormal();
		const FVector Eye = Pose.GetLocation() - Pose.GetRotation().GetForwardVector() * kFollowBackM * 100.0 + Up * kFollowUpM * 100.0;
		const FRotator Look = FRotationMatrix::MakeFromXZ(Pose.GetLocation() - Eye, Up).Rotator();
		Pawn->SetActorLocationAndRotation(Eye, Look);
		Controller->SetControlRotation(Look);
		return;
	}
}

void UFireSuppressionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AFireSimulator* Sim = Simulator();
	if (!Sim)
	{
		return;
	}
	if (SavedInstance && SimulatedMinutes > 0)
	{
		SavedInstance->SetScalarParameterValue(TEXT("Threshold"),
			static_cast<float>((FMath::Clamp(NowMinutes() / SimulatedMinutes, 0.0, 1.0) * 254.0 + 0.5) / 255.0));
	}
	RefreshActionsOverlay();
	if (!Aircraft.IsEmpty() || (WaterSpray && WaterSpray->GetInstanceCount() > 0) || (RetardantSpray && RetardantSpray->GetInstanceCount() > 0))
	{
		UpdateAircraft(DeltaTime);
		UpdateCamera();
	}
}

UMaterialInstanceDynamic* UFireSuppressionComponent::MakeOverlayInstance(float Opacity)
{
	const AFireSimulator* Sim = Simulator();
	UMaterialInterface* Parent = Sim ? Sim->OverlayMaterial.LoadSynchronous() : nullptr;
	if (!Parent)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, this);
	Instance->SetScalarParameterValue(TEXT("FrontWidth"), 1e-4f);  // pas de front : la couleur de la texture
	Instance->SetScalarParameterValue(TEXT("Opacity"), Opacity);
	return Instance;
}

void UFireSuppressionComponent::RebuildOverlayWindow()
{
	WindowColumns = WindowRows = 0;
	if (!Land)
	{
		if (ActionsDecal)
		{
			ActionsDecal->SetVisibility(false);
		}
		return;
	}
	const firesim::GridInfo& Grid = Land->grid;
	int32 ColMin = TNumericLimits<int32>::Max(), RowMin = ColMin, ColMax = TNumericLimits<int32>::Lowest(), RowMax = ColMax;
	auto Include = [&](uint32 Cell)
	{
		const int32 Col = static_cast<int32>(Cell % Grid.width), Row = static_cast<int32>(Cell / Grid.width);
		ColMin = FMath::Min(ColMin, Col);
		ColMax = FMath::Max(ColMax, Col);
		RowMin = FMath::Min(RowMin, Row);
		RowMax = FMath::Max(RowMax, Row);
	};
	for (const firesim::Treatment& Treatment : Treatments)
	{
		Include(Treatment.cell);
	}
	const std::vector<firesim::Lambert93> DraftPath(Draft.GetData(), Draft.GetData() + Draft.Num());
	for (const uint32 Cell : firesim::cells_along(Grid, DraftPath))
	{
		Include(Cell);
	}
	if (Draft.Num() == 1)
	{
		if (const auto Cell = Grid.cell_at(Draft[0].x, Draft[0].y))
		{
			Include(static_cast<uint32>(Grid.index(Cell->col, Cell->row)));
		}
	}
	if (ColMin > ColMax)
	{
		if (ActionsDecal)
		{
			ActionsDecal->SetVisibility(false);
		}
		return;
	}
	WindowCol = FMath::Max(0, ColMin - kWindowMargin);
	WindowRow = FMath::Max(0, RowMin - kWindowMargin);
	WindowColumns = FMath::Min(Grid.width - 1, ColMax + kWindowMargin) - WindowCol + 1;
	WindowRows = FMath::Min(Grid.height - 1, RowMax + kWindowMargin) - WindowRow + 1;
	WindowMinElevation = TNumericLimits<double>::Max();
	WindowMaxElevation = TNumericLimits<double>::Lowest();
	for (int32 Row = 0; Row < WindowRows; ++Row)
	{
		for (int32 Col = 0; Col < WindowColumns; ++Col)
		{
			if (const float Height = (*Elevation)[Grid.index(WindowCol + Col, WindowRow + Row)]; FMath::IsFinite(Height))
			{
				WindowMinElevation = FMath::Min(WindowMinElevation, static_cast<double>(Height));
				WindowMaxElevation = FMath::Max(WindowMaxElevation, static_cast<double>(Height));
			}
		}
	}
	if (WindowMinElevation > WindowMaxElevation)
	{
		WindowMinElevation = WindowMaxElevation = 0;
	}
	ActionsTexture = nullptr;  // autre taille : refaite au prochain affichage
	ActionPixels.Reset();
	RefreshActionsOverlay();
}

void UFireSuppressionComponent::RefreshActionsOverlay()
{
	if (WindowColumns == 0 || !Land)
	{
		return;
	}
	const firesim::GridInfo& Grid = Land->grid;
	const double Now = NowMinutes();
	TArray<FColor> Pixels;
	Pixels.Init(FColor(0, 0, 0, 255), WindowColumns * WindowRows);
	auto Paint = [&](uint32 Cell, const FColor& Color, bool bDashed)
	{
		const int32 Col = static_cast<int32>(Cell % Grid.width), Row = static_cast<int32>(Cell / Grid.width);
		if (bDashed && (Col + Row) % 2 == 0)
		{
			return;
		}
		const int32 X = Col - WindowCol, Y = Row - WindowRow;
		if (X >= 0 && Y >= 0 && X < WindowColumns && Y < WindowRows)
		{
			Pixels[Y * WindowColumns + X] = FColor(Color.R, Color.G, Color.B, 0);
		}
	};
	for (const firesim::Treatment& T : Treatments)
	{
		if (T.line >= 0)
		{
			const float Lost = T.line < static_cast<int32>(Lines.size()) ? Lines[T.line].cut_from : std::numeric_limits<float>::infinity();
			if (T.from_min >= Lost)
			{
				Paint(T.cell, kLost, true);
			}
			else if (T.from_min <= Now)
			{
				Paint(T.cell, kBuilt, false);
			}
			else
			{
				Paint(T.cell, kPlanned, true);
			}
			continue;
		}
		const bool bRetardant = std::isfinite(T.hold_min);
		if (T.from_min > Now)
		{
			Paint(T.cell, bRetardant ? kPlannedRetardant : kPlannedWater, true);
		}
		else if (Now < T.until_min)
		{
			Paint(T.cell, bRetardant ? kRetardant : kWater, false);
		}
	}
	const std::vector<firesim::Lambert93> DraftPath(Draft.GetData(), Draft.GetData() + Draft.Num());
	for (const uint32 Cell : firesim::cells_along(Grid, DraftPath))
	{
		Paint(Cell, kDraftColor, false);
	}
	if (Draft.Num() == 1)
	{
		if (const auto Cell = Grid.cell_at(Draft[0].x, Draft[0].y))
		{
			Paint(static_cast<uint32>(Grid.index(Cell->col, Cell->row)), kDraftColor, false);
		}
	}
	if (ActionsTexture && Pixels == ActionPixels)
	{
		return;
	}
	ActionPixels = MoveTemp(Pixels);
	ActionsTexture = MakeTexture(WindowColumns, WindowRows, ActionPixels);
	if (!ActionsInstance)
	{
		ActionsInstance = MakeOverlayInstance(0.95f);
		if (!ActionsInstance)
		{
			return;
		}
		ActionsInstance->SetScalarParameterValue(TEXT("Threshold"), 0.5f);  // seuil fixe : alpha 0 visible, 255 caché
	}
	ActionsInstance->SetTextureParameterValue(TEXT("FireTexture"), ActionsTexture);
	if (!ActionsDecal)
	{
		ActionsDecal = Simulator()->MakeDecal(TEXT("LutteDecal"));
		ActionsDecal->SetSortOrder(2);  // par-dessus le feu et le contour observé
		ActionsDecal->SetDecalMaterial(ActionsInstance);
	}
	ActionsDecal->SetVisibility(true);
	PlaceOverlays();
}

void UFireSuppressionComponent::BuildSavedOverlay(const firesim::SpreadResult& Fought, const firesim::SpreadResult& Free, double DurationMinutes)
{
	const firesim::GridInfo& Grid = Land->grid;
	SavedColumns = Free.col_max - Free.col_min + 1;
	SavedRows = Free.row_max - Free.row_min + 1;
	if (SavedColumns <= 0 || SavedRows <= 0)
	{
		return;
	}
	SavedWest = Grid.left + Free.col_min * Grid.res;
	SavedNorth = Grid.top - Free.row_min * Grid.res;

	TArray<float> FoughtArrival;
	FoughtArrival.Init(std::numeric_limits<float>::infinity(), SavedColumns * SavedRows);
	auto PixelOf = [&](uint32 Cell) -> int32
	{
		const int32 X = static_cast<int32>(Cell % Grid.width) - Free.col_min, Y = static_cast<int32>(Cell / Grid.width) - Free.row_min;
		return X >= 0 && Y >= 0 && X < SavedColumns && Y < SavedRows ? Y * SavedColumns + X : INDEX_NONE;
	};
	for (const firesim::BurnedCell& Cell : Fought.cells)
	{
		if (const int32 Pixel = PixelOf(Cell.index); Pixel != INDEX_NONE)
		{
			FoughtArrival[Pixel] = Cell.arrival_min;
		}
	}
	TArray<FColor> Pixels;
	Pixels.Init(FColor(0, 0, 0, 255), SavedColumns * SavedRows);
	SavedMinElevation = TNumericLimits<double>::Max();
	SavedMaxElevation = TNumericLimits<double>::Lowest();
	for (const firesim::BurnedCell& Cell : Free.cells)
	{
		const int32 Pixel = PixelOf(Cell.index);
		if (Pixel != INDEX_NONE && FoughtArrival[Pixel] > Cell.arrival_min + 1.0f)
		{
			const uint8 Alpha = static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(Cell.arrival_min / DurationMinutes, 0.0, 1.0) * 254.0));
			Pixels[Pixel] = FColor(kSavedColor.R, kSavedColor.G, kSavedColor.B, Alpha);
		}
		if (const float Height = (*Elevation)[Cell.index]; FMath::IsFinite(Height))
		{
			SavedMinElevation = FMath::Min(SavedMinElevation, static_cast<double>(Height));
			SavedMaxElevation = FMath::Max(SavedMaxElevation, static_cast<double>(Height));
		}
	}
	if (SavedMinElevation > SavedMaxElevation)
	{
		SavedMinElevation = SavedMaxElevation = 0;
	}
	SavedTexture = MakeTexture(SavedColumns, SavedRows, Pixels);
	if (!SavedInstance)
	{
		SavedInstance = MakeOverlayInstance(0.6f);
		if (!SavedInstance)
		{
			return;
		}
	}
	SavedInstance->SetTextureParameterValue(TEXT("FireTexture"), SavedTexture);
	if (!SavedDecal)
	{
		SavedDecal = Simulator()->MakeDecal(TEXT("SauveDecal"));
		SavedDecal->SetSortOrder(-1);  // sous le feu : une cellule retardée redevient feu quand il arrive
		SavedDecal->SetDecalMaterial(SavedInstance);
	}
	SavedDecal->SetVisibility(true);
	PlaceOverlays();
}

void UFireSuppressionComponent::PlaceOverlays()
{
	AFireSimulator* Sim = Simulator();
	if (!Sim || !Land)
	{
		return;
	}
	if (ActionsDecal && WindowColumns > 0)
	{
		FFireDecalWindow Window;
		Window.Columns = WindowColumns;
		Window.Rows = WindowRows;
		Window.CellSize = Land->grid.res;
		Window.West = Land->grid.left + WindowCol * Land->grid.res;
		Window.North = Land->grid.top - WindowRow * Land->grid.res;
		Window.MinElevation = WindowMinElevation;
		Window.MaxElevation = WindowMaxElevation;
		Sim->PlaceDecal(ActionsDecal, Window);
	}
	if (SavedDecal && SavedColumns > 0)
	{
		FFireDecalWindow Window;
		Window.Columns = SavedColumns;
		Window.Rows = SavedRows;
		Window.CellSize = Land->grid.res;
		Window.West = SavedWest;
		Window.North = SavedNorth;
		Window.MinElevation = SavedMinElevation;
		Window.MaxElevation = SavedMaxElevation;
		Sim->PlaceDecal(SavedDecal, Window);
	}
}

void UFireSuppressionComponent::CreateInterface(UGameViewportClient& Viewport)
{
	TWeakObjectPtr<AFireSimulator> Owner(Simulator());
	Interface = SNew(SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(16.f)
		.Visibility_Lambda([Owner]
		{
			return Owner.IsValid() && Owner->bShowInterface ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed;
		})
		[
			SNew(SFireLuttePanel, this)
		];
	Viewport.AddViewportWidgetContent(Interface.ToSharedRef(), 10);
}

void UFireSuppressionComponent::EndPlay(const EEndPlayReason::Type Reason)
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

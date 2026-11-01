#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class AFireSimulator;
enum class EFireMoisture : uint8;

// Panneau de réglage affiché par-dessus la vue en jeu : vent, humidité des combustibles et durée (le feu est
// recalculé au relâchement d'un curseur), puis lecture du feu heure par heure.
class SFireSimPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFireSimPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, AFireSimulator* InSimulator);

	// Les clics et la molette sur le panneau ne doivent pas atteindre la caméra, derrière.
	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;

private:
	TSharedRef<SWidget> Section(const FText& Title) const;
	TSharedRef<SWidget> ValueRow(const FText& Label, TAttribute<FText> Value) const;
	TSharedRef<SWidget> MoistureChoice(EFireMoisture Moisture, const FText& Label);
	TSharedRef<SWidget> WeatherChoice(bool bReal, const FText& Label);
	// Décale l'heure de départ du feu (météo réelle) et relance le calcul.
	void ShiftIgnition(FTimespan Delta);
	void SetIgnitionDate(const FText& Text);
	void Resimulate();

	TWeakObjectPtr<AFireSimulator> Simulator;
	FSlateRoundedBoxBrush Background{FLinearColor(0.015f, 0.015f, 0.02f, 0.78f), 10.0f};
};

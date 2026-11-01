#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"

class UFireSuppressionComponent;
enum class EFireTool : uint8;

// Panneau de la lutte, à droite de la vue : outils (pare-feu, Canadair, Dash), ordres en cours et hectares évités.
class SFireLuttePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFireLuttePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UFireSuppressionComponent* InLutte);

	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override;
	// Les clics et la molette sur le panneau ne doivent pas atteindre la caméra, derrière.
	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;

private:
	TSharedRef<SWidget> ToolButton(EFireTool Tool, const FText& Label, const FText& ToolTip);
	TSharedRef<SWidget> WidthChoice(double WidthM);
	TSharedRef<SWidget> EnginesChoice(int32 Engines);
	void RebuildList();

	TWeakObjectPtr<UFireSuppressionComponent> Lutte;
	TSharedPtr<SVerticalBox> List;
	int32 ShownRevision = -1;
	FSlateRoundedBoxBrush Background{FLinearColor(0.015f, 0.015f, 0.02f, 0.78f), 10.0f};
};

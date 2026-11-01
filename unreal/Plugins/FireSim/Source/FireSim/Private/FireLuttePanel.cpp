#include "FireLuttePanel.h"

#include "FireSuppression.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
const FLinearColor kMuted(0.62f, 0.62f, 0.66f);
const FLinearColor kAccent(1.0f, 0.62f, 0.25f);
const FLinearColor kError(1.0f, 0.35f, 0.3f);
}  // namespace

void SFireLuttePanel::Construct(const FArguments& InArgs, UFireSuppressionComponent* InLutte)
{
	Lutte = InLutte;
	const FSlateFontInfo Small = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	const FSlateFontInfo Bold = FCoreStyle::GetDefaultFontStyle("Bold", 9);
	auto Get = [this] { return Lutte.Get(); };
	auto FirebreakOnly = [Get] { return Get() && Get()->GetTool() == EFireTool::Firebreak ? EVisibility::Visible : EVisibility::Collapsed; };
	auto ToolActive = [Get] { return Get() && Get()->GetTool() != EFireTool::None ? EVisibility::Visible : EVisibility::Collapsed; };

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(330.f)
		[
			SNew(SBorder)
			.BorderImage(&Background)
			.Padding(FMargin(16.f, 12.f, 16.f, 14.f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(INVTEXT("LUTTE")).Font(Bold).ColorAndOpacity(kAccent)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SUniformGridPanel)
					.SlotPadding(FMargin(1.f))
					+ SUniformGridPanel::Slot(0, 0)
					[
						ToolButton(EFireTool::Firebreak, INVTEXT("Pare-feu"), INVTEXT("Tracer une ligne que des engins vont construire"))
					]
					+ SUniformGridPanel::Slot(1, 0)
					[
						ToolButton(EFireTool::Canadair, INVTEXT("Canadair"), INVTEXT("Largages d'eau : écope en mer, retient le feu le temps que l'eau sèche"))
					]
					+ SUniformGridPanel::Slot(2, 0)
					[
						ToolButton(EFireTool::Dash, INVTEXT("Dash"), INVTEXT("Largages de retardant rouge : se pose avant le feu, le retient une fois au contact"))
					]
				]

				// Réglages du pare-feu
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda(FirebreakOnly)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox).WidthOverride(58.f)[SNew(STextBlock).Font(Small).ColorAndOpacity(kMuted).Text(INVTEXT("Largeur"))]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f)
					[
						SNew(SUniformGridPanel)
						+ SUniformGridPanel::Slot(0, 0)[WidthChoice(4)]
						+ SUniformGridPanel::Slot(1, 0)[WidthChoice(8)]
						+ SUniformGridPanel::Slot(2, 0)[WidthChoice(16)]
						+ SUniformGridPanel::Slot(3, 0)[WidthChoice(32)]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda(FirebreakOnly)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox).WidthOverride(58.f)[SNew(STextBlock).Font(Small).ColorAndOpacity(kMuted).Text(INVTEXT("Engins"))]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f)
					[
						SNew(SUniformGridPanel)
						+ SUniformGridPanel::Slot(0, 0)[EnginesChoice(1)]
						+ SUniformGridPanel::Slot(1, 0)[EnginesChoice(2)]
						+ SUniformGridPanel::Slot(2, 0)[EnginesChoice(4)]
						+ SUniformGridPanel::Slot(3, 0)[EnginesChoice(8)]
					]
				]

				// Consigne et tracé en cours
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Visibility_Lambda(ToolActive)
					.AutoWrapText(true)
					.Font(Small)
					.ColorAndOpacity(kMuted)
					.Text_Lambda([Get] { return FText::FromString(Get() ? Get()->DraftText() : FString()); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([Get] { return Get() && Get()->DraftPointCount() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 2.f, 0.f)
					[
						SNew(SButton)
						.IsFocusable(false)
						.HAlign(HAlign_Center)
						.Visibility_Lambda(FirebreakOnly)
						.IsEnabled_Lambda([Get] { return Get() && Get()->DraftPointCount() >= 2; })
						.Text(INVTEXT("Lancer le chantier"))
						.ToolTipText(INVTEXT("Clic droit ou Entrée"))
						.OnClicked_Lambda([Get] { if (Get()) Get()->FinishDraft(); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.IsFocusable(false)
						.HAlign(HAlign_Center)
						.Text(INVTEXT("Retirer le point"))
						.ToolTipText(INVTEXT("Retour arrière"))
						.OnClicked_Lambda([Get] { if (Get()) Get()->RemoveLastDraftPoint(); return FReply::Handled(); })
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Font(Small)
					.ColorAndOpacity(kError)
					.Visibility_Lambda([Get] { return Get() && !Get()->GetLastError().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text_Lambda([Get] { return FText::FromString(Get() ? Get()->GetLastError() : FString()); })
				]

				// Ordres en cours
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SAssignNew(List, SVerticalBox)
				]

				// Bilan
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility_Lambda([Get] { return Get() && Get()->ActionCount() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text_Lambda([Get] { return FText::FromString(Get() ? Get()->ComparisonText() : FString()); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SCheckBox)
					.IsFocusable(false)
					.IsChecked_Lambda([Get] { return Get() && Get()->bSlowDrops ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([Get](ECheckBoxState State) { if (Get()) Get()->bSlowDrops = State == ECheckBoxState::Checked; })
					[
						SNew(STextBlock).Font(Small).Text(INVTEXT("Ralentir pendant les largages"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Font(Small)
					.ColorAndOpacity(kMuted)
					.Text(INVTEXT("Efficacité indicative. Pas de sautes de feu : par grand vent, les pare-feu sont optimistes."))
				]
			]
		]
	];
	RebuildList();
}

void SFireLuttePanel::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
	SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
	if (const UFireSuppressionComponent* Component = Lutte.Get(); Component && Component->GetRevision() != ShownRevision)
	{
		RebuildList();
	}
}

void SFireLuttePanel::RebuildList()
{
	UFireSuppressionComponent* Component = Lutte.Get();
	if (!List || !Component)
	{
		return;
	}
	ShownRevision = Component->GetRevision();
	List->ClearChildren();
	const FSlateFontInfo Small = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	auto Get = [this] { return Lutte.Get(); };
	for (int32 I = 0; I < Component->ActionCount(); ++I)
	{
		const FString Callsign = Component->FlightOfAction(I);
		List->AddSlot().AutoHeight().Padding(0.f, 3.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(Small)
				.Text_Lambda([Get, I] { return FText::FromString(Get() ? Get()->ActionText(I) : FString()); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.IsFocusable(false)
				.Visibility(Callsign.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.ToolTipText(INVTEXT("Caméra derrière cet avion"))
				.ButtonColorAndOpacity_Lambda([Get, Callsign]
				{
					return Get() && Get()->GetFollowedFlight() == Callsign ? FSlateColor(kAccent) : FSlateColor(FLinearColor::White);
				})
				.OnClicked_Lambda([Get, Callsign]
				{
					if (Get())
					{
						Get()->FollowFlight(Get()->GetFollowedFlight() == Callsign ? FString() : Callsign);
					}
					return FReply::Handled();
				})
				[
					SNew(STextBlock).Font(Small).Text(INVTEXT("Suivre"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.IsFocusable(false)
				.ToolTipText(INVTEXT("Annuler cet ordre"))
				.OnClicked_Lambda([Get, I] { if (Get()) Get()->RemoveAction(I); return FReply::Handled(); })
				[
					SNew(STextBlock).Font(Small).Text(INVTEXT("X"))
				]
			]
		];
	}
}

FReply SFireLuttePanel::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
	return FReply::Handled();
}

FReply SFireLuttePanel::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
	return FReply::Handled();
}

TSharedRef<SWidget> SFireLuttePanel::ToolButton(EFireTool Tool, const FText& Label, const FText& ToolTip)
{
	return SNew(SButton)
		.IsFocusable(false)
		.HAlign(HAlign_Center)
		.ToolTipText(ToolTip)
		.ButtonColorAndOpacity_Lambda([this, Tool]
		{
			const UFireSuppressionComponent* Component = Lutte.Get();
			return Component && Component->GetTool() == Tool ? FSlateColor(kAccent) : FSlateColor(FLinearColor::White);
		})
		.OnClicked_Lambda([this, Tool]
		{
			if (UFireSuppressionComponent* Component = Lutte.Get())
			{
				Component->SetTool(Tool);
			}
			return FReply::Handled();
		})
		[
			SNew(STextBlock).Text(Label)
		];
}

TSharedRef<SWidget> SFireLuttePanel::WidthChoice(double WidthM)
{
	return SNew(SCheckBox)
		.Style(&FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
		.IsFocusable(false)
		.IsChecked_Lambda([this, WidthM]
		{
			const UFireSuppressionComponent* Component = Lutte.Get();
			return Component && Component->DraftWidthM == WidthM ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, WidthM](ECheckBoxState)
		{
			if (UFireSuppressionComponent* Component = Lutte.Get())
			{
				Component->DraftWidthM = WidthM;
			}
		})
		[
			SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).Text(FText::FromString(FString::Printf(TEXT("%.0f m"), WidthM)))
		];
}

TSharedRef<SWidget> SFireLuttePanel::EnginesChoice(int32 Engines)
{
	return SNew(SCheckBox)
		.Style(&FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
		.IsFocusable(false)
		.IsChecked_Lambda([this, Engines]
		{
			const UFireSuppressionComponent* Component = Lutte.Get();
			return Component && Component->DraftEngines == Engines ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Engines](ECheckBoxState)
		{
			if (UFireSuppressionComponent* Component = Lutte.Get())
			{
				Component->DraftEngines = Engines;
			}
		})
		[
			SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).Text(FText::AsNumber(Engines))
		];
}

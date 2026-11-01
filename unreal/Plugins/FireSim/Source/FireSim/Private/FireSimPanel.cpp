#include "FireSimPanel.h"

#include "FireSimulator.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
const FLinearColor kMuted(0.62f, 0.62f, 0.66f);
const FLinearColor kAccent(1.0f, 0.62f, 0.25f);

// Vitesse de lecture, en heures de feu par seconde : échelle logarithmique sur le curseur.
constexpr double kMinPlaybackSpeed = 0.05, kMaxPlaybackSpeed = 2.0;

FString OneDecimal(double Value)
{
	return FString::Printf(TEXT("%.1f"), Value).Replace(TEXT("."), TEXT(","));
}

// « 2 h 05 »
FString Clock(double Hours)
{
	const int32 Minutes = FMath::RoundToInt(Hours * 60.0);
	return FString::Printf(TEXT("%d h %02d"), Minutes / 60, Minutes % 60);
}

// Direction d'où vient le vent, avec le nom du vent en Corse.
FString WindOrigin(double FromDegrees)
{
	static const TCHAR* Names[] = {TEXT("du nord (tramontane)"), TEXT("du nord-est (grecale)"), TEXT("de l'est (levante)"),
		TEXT("du sud-est (sirocco)"), TEXT("du sud (ostro)"), TEXT("du sud-ouest (libeccio)"), TEXT("de l'ouest (ponente)"),
		TEXT("du nord-ouest (mistral)")};
	const int32 Sector = FMath::RoundToInt(FMath::Fmod(FromDegrees + 360.0, 360.0) / 45.0) % 8;
	return FString::Printf(TEXT("%s, %.0f°"), Names[Sector], FromDegrees);
}
}  // namespace

void SFireSimPanel::Construct(const FArguments& InArgs, AFireSimulator* InSimulator)
{
	Simulator = InSimulator;
	const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle("Bold", 15);
	const FSlateFontInfo Small = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	auto Sim = [this] { return Simulator.Get(); };
	auto RealOnly = [Sim] { return Sim() && Sim()->bRealWeather ? EVisibility::Visible : EVisibility::Collapsed; };
	// Météo réelle à l'heure affichée du feu, si le feu a été calculé avec
	auto Now = [Sim](FFireWeatherNow& Out) { return Sim() && Sim()->RealWeatherAt(Sim()->DisplayedHours, Out); };

	// Un bouton par feu réel du catalogue, sur deux colonnes ; celui qui est rejoué ressort en orange.
	const TSharedRef<SUniformGridPanel> ReplayButtons = SNew(SUniformGridPanel).SlotPadding(FMargin(1.f));
	const TArray<FFireReplay> Catalogue = InSimulator ? InSimulator->GetReplays() : TArray<FFireReplay>();
	for (int32 I = 0; I < Catalogue.Num(); ++I)
	{
		const FFireReplay& Replay = Catalogue[I];
		const FString Id = Replay.Id;
		ReplayButtons->AddSlot(I % 2, I / 2)
		[
			SNew(SButton)
			.IsFocusable(false)
			.HAlign(HAlign_Center)
			.ButtonColorAndOpacity_Lambda([Sim, Id]
			{
				const FFireReplay* Active = Sim() ? Sim()->GetActiveReplay() : nullptr;
				return Active && Active->Id == Id ? FSlateColor(kAccent) : FSlateColor(FLinearColor::White);
			})
			.ToolTipText(FText::FromString(FString::Printf(TEXT("%s, départ le %s"), *Replay.Name,
				*Replay.Start.ToString(TEXT("%d/%m/%Y à %H h")))))
			.OnClicked_Lambda([Sim, Id]
			{
				if (Sim())
				{
					Sim()->StartReplay(Id);
				}
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Font(Small).Text(FText::FromString(
					FString::Printf(TEXT("%s %d"), *Replay.ShortName, Replay.Start.GetYear())))
			]
		];
	}

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(340.f)
		[
			SNew(SBorder)
			.BorderImage(&Background)
			.Padding(FMargin(16.f, 12.f, 16.f, 14.f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(INVTEXT("Feu de forêt en Corse")).Font(Title)
				]

				// Feux réels
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox)
					.Visibility(InSimulator && !InSimulator->GetReplays().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed)
					[
						Section(INVTEXT("REJOUER UN FEU RÉEL"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight()[ReplayButtons]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Visibility_Lambda([Sim]
					{
						return Sim() && Sim()->GetActiveReplay() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.AutoWrapText(true)
					.Font(Small)
					.ColorAndOpacity(kMuted)
					.Text_Lambda([Sim, Now]
					{
						const FFireReplay* Replay = Sim() ? Sim()->GetActiveReplay() : nullptr;
						if (!Replay)
						{
							return FText::GetEmpty();
						}
						FString Text = FString::Printf(TEXT("%s, départ le %s.\nContour bleu : zone réellement brûlée (EFFIS), %.0f ha."),
							*Replay->Name, *Replay->Start.ToString(TEXT("%d/%m/%Y à %H h")), Replay->ObservedHa);
						double SimulatedHa = 0, Sorensen = 0;
						if (Sim()->ReplayScoresAt(Sim()->DisplayedHours, SimulatedHa, Sorensen))
						{
							FFireWeatherNow Real;
							const FString When = Now(Real) ? Real.Time.ToString(TEXT("%H h %M")) : Clock(Sim()->DisplayedHours);
							Text += FString::Printf(TEXT("\nÀ %s : simulé %.0f ha, recouvrement %s (1 = identique)"), *When, SimulatedHa,
								*FString::Printf(TEXT("%.2f"), Sorensen).Replace(TEXT("."), TEXT(",")));
						}
						for (int32 I = 0; I < Replay->Reported.Num(); ++I)
						{
							Text += FString::Printf(TEXT("%s%s : %.0f ha"), I ? TEXT(" ; ") : TEXT("\nAnnoncé : "),
								*Replay->Reported[I].Key.ToString(TEXT("%d/%m %H h %M")), Replay->Reported[I].Value);
						}
						return FText::FromString(Text);
					})
				]

				// Météo
				+ SVerticalBox::Slot().AutoHeight()[Section(INVTEXT("MÉTÉO"))]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f)[WeatherChoice(false, INVTEXT("Réglée à la main"))]
					+ SHorizontalBox::Slot().FillWidth(1.f)[WeatherChoice(true, INVTEXT("Réelle"))]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda(RealOnly)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.IsFocusable(false)
						.Text(INVTEXT("<"))
						.ToolTipText(INVTEXT("La veille"))
						.OnClicked_Lambda([this] { ShiftIgnition(FTimespan::FromDays(-1)); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(4.f, 0.f)
					[
						SNew(SEditableTextBox)
						.Justification(ETextJustify::Center)
						.ToolTipText(INVTEXT("Date du départ du feu (jour/mois/année), puis Entrée"))
						.Text_Lambda([Sim]
						{
							return FText::FromString(Sim() ? Sim()->IgnitionTime.ToString(TEXT("%d/%m/%Y")) : FString());
						})
						.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type) { SetIgnitionDate(Text); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.IsFocusable(false)
						.Text(INVTEXT(">"))
						.ToolTipText(INVTEXT("Le lendemain"))
						.OnClicked_Lambda([this] { ShiftIgnition(FTimespan::FromDays(1)); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.IsFocusable(false)
						.Text(INVTEXT("Maintenant"))
						.OnClicked_Lambda([this, Sim]
						{
							if (Sim())
							{
								const FDateTime Current = FDateTime::Now();
								ShiftIgnition(FDateTime(Current.GetYear(), Current.GetMonth(), Current.GetDay(), Current.GetHour()) -
									Sim()->IgnitionTime);
							}
							return FReply::Handled();
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SBox)
					.Visibility_Lambda(RealOnly)
					[
						ValueRow(INVTEXT("Départ du feu"), TAttribute<FText>::CreateLambda([Sim]
						{
							return FText::FromString(Sim() ? FString::Printf(TEXT("%d h"), Sim()->IgnitionTime.GetHour()) : FString());
						}))
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda(RealOnly)
					[
						SNew(SSlider)
						.MinValue(0.f).MaxValue(23.f).StepSize(1.f).MouseUsesStep(true).IsFocusable(false)
						.Value_Lambda([Sim] { return Sim() ? static_cast<float>(Sim()->IgnitionTime.GetHour()) : 0.f; })
						.OnValueChanged_Lambda([Sim](float Value)
						{
							if (AFireSimulator* S = Sim())
							{
								const FDateTime Day = S->IgnitionTime.GetDate();
								S->IgnitionTime = Day + FTimespan::FromHours(FMath::RoundToInt(Value));
							}
						})
						.OnMouseCaptureEnd(this, &SFireSimPanel::Resimulate)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Visibility_Lambda(RealOnly)
					.AutoWrapText(true)
					.Font(Small)
					.ColorAndOpacity(kMuted)
					.Text_Lambda([Sim]
					{
						if (!Sim())
						{
							return FText::GetEmpty();
						}
						if (Sim()->IsLoadingWeather())
						{
							return INVTEXT("Chargement de la météo…");
						}
						return FText::FromString(Sim()->WeatherSource.IsEmpty()
							? FString(TEXT("Prévisions et archives de Météo-France (depuis novembre 2022), sinon ERA5 (depuis 1940)"))
							: FString::Printf(TEXT("Source : %s, via Open-Meteo"), *Sim()->WeatherSource));
					})
				]

				// Vent : réglé à la main, ou celui de la météo réelle à l'heure affichée
				+ SVerticalBox::Slot().AutoHeight()[Section(INVTEXT("VENT"))]
				+ SVerticalBox::Slot().AutoHeight()
				[
					ValueRow(INVTEXT("Vitesse"), TAttribute<FText>::CreateLambda([Sim, Now]
					{
						FFireWeatherNow Real;
						const double Speed = Now(Real) ? Real.WindKmh : Sim() ? Sim()->WindSpeedKmh : 0.0;
						return FText::FromString(FString::Printf(TEXT("%.0f km/h"), Speed));
					}))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SSlider)
					.MinValue(0.f).MaxValue(120.f).StepSize(5.f).MouseUsesStep(true).IsFocusable(false)
					.IsEnabled_Lambda([Sim] { return Sim() && !Sim()->bRealWeather; })
					.Value_Lambda([Sim, Now]
					{
						FFireWeatherNow Real;
						return static_cast<float>(Now(Real) ? Real.WindKmh : Sim() ? Sim()->WindSpeedKmh : 0.0);
					})
					.OnValueChanged_Lambda([Sim](float Value) { if (Sim()) Sim()->WindSpeedKmh = Value; })
					.OnMouseCaptureEnd(this, &SFireSimPanel::Resimulate)
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					ValueRow(INVTEXT("Vient"), TAttribute<FText>::CreateLambda([Sim, Now]
					{
						FFireWeatherNow Real;
						const double From = Now(Real) ? Real.WindFromDeg : Sim() ? Sim()->WindFromDegrees : 0.0;
						return FText::FromString(WindOrigin(From));
					}))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SSlider)
					.MinValue(0.f).MaxValue(360.f).StepSize(5.f).MouseUsesStep(true).IsFocusable(false)
					.IsEnabled_Lambda([Sim] { return Sim() && !Sim()->bRealWeather; })
					.Value_Lambda([Sim, Now]
					{
						FFireWeatherNow Real;
						return static_cast<float>(Now(Real) ? Real.WindFromDeg : Sim() ? Sim()->WindFromDegrees : 0.0);
					})
					.OnValueChanged_Lambda([Sim](float Value) { if (Sim()) Sim()->WindFromDegrees = Value; })
					.OnMouseCaptureEnd(this, &SFireSimPanel::Resimulate)
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda([Now]
					{
						FFireWeatherNow Real;
						return Now(Real) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						ValueRow(INVTEXT("Air"), TAttribute<FText>::CreateLambda([Now]
						{
							FFireWeatherNow Real;
							Now(Real);
							return FText::FromString(FString::Printf(TEXT("%.0f °C, %.0f %% d'humidité, brindilles à %s %%"),
								Real.TemperatureC, Real.HumidityPct, *OneDecimal(Real.Dead1h * 100.0)));
						}))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SCheckBox)
					.IsFocusable(false)
					.IsChecked_Lambda([Sim]
					{
						return Sim() && Sim()->bTerrainWind ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this, Sim](ECheckBoxState State)
					{
						if (Sim())
						{
							Sim()->bTerrainWind = State == ECheckBoxState::Checked;
							Resimulate();
						}
					})
					[
						SNew(STextBlock).Text(INVTEXT("Vent corrigé par le relief"))
					]
				]

				// Combustibles
				+ SVerticalBox::Slot().AutoHeight()[Section(INVTEXT("SÉCHERESSE DE LA VÉGÉTATION"))]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SUniformGridPanel)
					.SlotPadding(FMargin(0.f, 2.f, 8.f, 2.f))
					+ SUniformGridPanel::Slot(0, 0)[MoistureChoice(EFireMoisture::Extreme, INVTEXT("Extrême"))]
					+ SUniformGridPanel::Slot(1, 0)[MoistureChoice(EFireMoisture::VeryDry, INVTEXT("Très forte"))]
					+ SUniformGridPanel::Slot(0, 1)[MoistureChoice(EFireMoisture::Dry, INVTEXT("Forte"))]
					+ SUniformGridPanel::Slot(1, 1)[MoistureChoice(EFireMoisture::Moderate, INVTEXT("Modérée"))]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda(RealOnly)
					.AutoWrapText(true)
					.Font(Small)
					.ColorAndOpacity(kMuted)
					.Text(INVTEXT("Météo réelle : le bois mort suit la météo, ce choix ne règle que la végétation vivante."))
				]

				// Simulation
				+ SVerticalBox::Slot().AutoHeight()[Section(INVTEXT("SIMULATION"))]
				+ SVerticalBox::Slot().AutoHeight()
				[
					ValueRow(INVTEXT("Durée"), TAttribute<FText>::CreateLambda([Sim]
					{
						return FText::FromString(Sim() ? FString::Printf(TEXT("%.0f h"), Sim()->DurationHours) : FString());
					}))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SSlider)
					.MinValue(1.f).MaxValue(24.f).StepSize(1.f).MouseUsesStep(true).IsFocusable(false)
					.Value_Lambda([Sim] { return Sim() ? static_cast<float>(Sim()->DurationHours) : 1.f; })
					.OnValueChanged_Lambda([Sim](float Value) { if (Sim()) Sim()->DurationHours = Value; })
					.OnMouseCaptureEnd(this, &SFireSimPanel::Resimulate)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 4.f, 0.f)
					[
						SNew(SButton)
						.IsFocusable(false)
						.HAlign(HAlign_Center)
						.Text(INVTEXT("Relancer le feu"))
						.OnClicked_Lambda([this] { Resimulate(); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(4.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.IsFocusable(false)
						.HAlign(HAlign_Center)
						.Text(INVTEXT("Effacer"))
						.OnClicked_Lambda([Sim] { if (Sim()) Sim()->ClearFire(); return FReply::Handled(); })
					]
				]

				// Lecture
				+ SVerticalBox::Slot().AutoHeight()[Section(INVTEXT("LECTURE"))]
				+ SVerticalBox::Slot().AutoHeight()
				[
					ValueRow(INVTEXT("Heure du feu"), TAttribute<FText>::CreateLambda([Sim, Now]
					{
						if (!Sim() || !Sim()->HasFire())
						{
							return INVTEXT("pas de feu");
						}
						const double Hours = Sim()->DisplayedHours;
						FFireWeatherNow Real;
						const FString When = Now(Real)
							? FString::Printf(TEXT("%s (+%s)"), *Real.Time.ToString(TEXT("%d/%m %H h %M")), *Clock(Hours))
							: Clock(Hours);
						return FText::FromString(FString::Printf(TEXT("%s · %.0f ha"), *When, Sim()->AreaAt(Hours)));
					}))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SSlider)
					.IsFocusable(false)
					.Value_Lambda([Sim]
					{
						const double Hours = Sim() ? Sim()->GetSimulatedHours() : 0.0;
						return Hours > 0.0 ? static_cast<float>(Sim()->DisplayedHours / Hours) : 0.f;
					})
					.OnMouseCaptureBegin_Lambda([Sim] { if (Sim()) Sim()->SetPlaying(false); })
					.OnValueChanged_Lambda([Sim](float Value)
					{
						if (Sim())
						{
							Sim()->SetDisplayedHours(Value * Sim()->GetSimulatedHours());
						}
					})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox)
						.WidthOverride(80.f)
						[
							SNew(SButton)
							.IsFocusable(false)
							.HAlign(HAlign_Center)
							.Text_Lambda([Sim]
							{
								if (Sim() && Sim()->IsPlaying())
								{
									return INVTEXT("Pause");
								}
								return Sim() && Sim()->DisplayedHours >= Sim()->GetSimulatedHours() ? INVTEXT("Rejouer") : INVTEXT("Lire");
							})
							.OnClicked_Lambda([Sim]
							{
								if (Sim())
								{
									Sim()->SetPlaying(!Sim()->IsPlaying());
								}
								return FReply::Handled();
							})
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Font(Small)
							.ColorAndOpacity(kMuted)
							.Text_Lambda([Sim]
							{
								const double Speed = Sim() ? Sim()->PlaybackSpeed : kMinPlaybackSpeed;
								return FText::FromString(FString::Printf(TEXT("1 h de feu en %s s"), *OneDecimal(1.0 / Speed)));
							})
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SSlider)
							.IsFocusable(false)
							.Value_Lambda([Sim]
							{
								const double Speed = FMath::Clamp(Sim() ? Sim()->PlaybackSpeed : kMinPlaybackSpeed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
								return static_cast<float>(FMath::Loge(Speed / kMinPlaybackSpeed) / FMath::Loge(kMaxPlaybackSpeed / kMinPlaybackSpeed));
							})
							.OnValueChanged_Lambda([Sim](float Value)
							{
								if (Sim())
								{
									Sim()->PlaybackSpeed = kMinPlaybackSpeed * FMath::Pow(kMaxPlaybackSpeed / kMinPlaybackSpeed, static_cast<double>(Value));
								}
							})
						]
					]
				]

				// Résultat du dernier calcul
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([Sim]
					{
						if (!Sim() || !Sim()->HasFire())
						{
							return FText::GetEmpty();
						}
						return FText::FromString(FString::Printf(TEXT("En %.0f h : %.0f ha, front jusqu'à %s km/h, flammes jusqu'à %s m"),
							Sim()->GetSimulatedHours(), Sim()->BurnedAreaHa, *OneDecimal(Sim()->MaxSpreadKmh),
							*OneDecimal(Sim()->MaxFlameLengthM)));
					})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Font(Small)
					.ColorAndOpacity(kMuted)
					.Text_Lambda([Sim] { return FText::FromString(Sim() ? Sim()->Status : FString()); })
				]

				// Commandes
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Font(Small)
					.ColorAndOpacity(kMuted)
					.Text(INVTEXT("F : allumer un feu sous la souris\n"
								  "Glisser dans la vue : regarder autour\n"
								  "Flèches : avancer, tourner ; E / C : monter, descendre\n"
								  "H : masquer ce panneau"))
				]
			]
		]
	];
}

FReply SFireSimPanel::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
	return FReply::Handled();
}

FReply SFireSimPanel::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
	return FReply::Handled();
}

TSharedRef<SWidget> SFireSimPanel::Section(const FText& Title) const
{
	return SNew(SBox)
		.Padding(FMargin(0.f, 12.f, 0.f, 2.f))
		[
			SNew(STextBlock)
			.Text(Title)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			.ColorAndOpacity(kAccent)
		];
}

TSharedRef<SWidget> SFireSimPanel::ValueRow(const FText& Label, TAttribute<FText> Value) const
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(STextBlock).Text(Label).ColorAndOpacity(kMuted)
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Right)
		[
			SNew(STextBlock).Text(Value)
		];
}

TSharedRef<SWidget> SFireSimPanel::MoistureChoice(EFireMoisture Moisture, const FText& Label)
{
	return SNew(SCheckBox)
		.Style(&FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
		.IsFocusable(false)
		.IsChecked_Lambda([this, Moisture]
		{
			const AFireSimulator* Sim = Simulator.Get();
			return Sim && Sim->Moisture == Moisture ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Moisture](ECheckBoxState)
		{
			AFireSimulator* Sim = Simulator.Get();
			if (Sim && Sim->Moisture != Moisture)
			{
				Sim->Moisture = Moisture;
				Resimulate();
			}
		})
		[
			SNew(STextBlock).Text(Label)
		];
}

TSharedRef<SWidget> SFireSimPanel::WeatherChoice(bool bReal, const FText& Label)
{
	return SNew(SCheckBox)
		.Style(&FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
		.IsFocusable(false)
		.IsChecked_Lambda([this, bReal]
		{
			const AFireSimulator* Sim = Simulator.Get();
			return Sim && Sim->bRealWeather == bReal ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, bReal](ECheckBoxState)
		{
			AFireSimulator* Sim = Simulator.Get();
			if (Sim && Sim->bRealWeather != bReal)
			{
				Sim->bRealWeather = bReal;
				Resimulate();
			}
		})
		[
			SNew(STextBlock).Text(Label)
		];
}

void SFireSimPanel::ShiftIgnition(FTimespan Delta)
{
	if (AFireSimulator* Sim = Simulator.Get())
	{
		Sim->IgnitionTime += Delta;
		Resimulate();
	}
}

void SFireSimPanel::SetIgnitionDate(const FText& Text)
{
	// La saisie a pris le clavier : on le rend au jeu (touches F, H, flèches).
	FSlateApplication::Get().SetAllUserFocusToGameViewport();
	AFireSimulator* Sim = Simulator.Get();
	TArray<FString> Parts;
	Text.ToString().TrimStartAndEnd().ParseIntoArray(Parts, TEXT("/"));
	if (!Sim || Parts.Num() != 3)
	{
		return;
	}
	const int32 Day = FCString::Atoi(*Parts[0]), Month = FCString::Atoi(*Parts[1]);
	int32 Year = FCString::Atoi(*Parts[2]);
	Year += Year < 100 ? 2000 : 0;
	const int32 Hour = Sim->IgnitionTime.GetHour();
	if (FDateTime::Validate(Year, Month, Day, Hour, 0, 0, 0))
	{
		ShiftIgnition(FDateTime(Year, Month, Day, Hour) - Sim->IgnitionTime);
	}
}

void SFireSimPanel::Resimulate()
{
	if (AFireSimulator* Sim = Simulator.Get())
	{
		Sim->Simulate();
	}
}

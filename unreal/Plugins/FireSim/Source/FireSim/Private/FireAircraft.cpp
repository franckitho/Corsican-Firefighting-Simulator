#include "FireAircraft.h"

#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

namespace
{
const FLinearColor kYellow(1.0f, 0.62f, 0.02f);
const FLinearColor kRed(0.72f, 0.03f, 0.02f);
const FLinearColor kDark(0.02f, 0.03f, 0.05f);
const FLinearColor kGrey(0.22f, 0.22f, 0.24f);

constexpr double kCmPerM = 100.0;
constexpr int32 kRingPoints = 16;

using FRing = TArray<FVector>;

// Maillage en construction, en mètres : X du nez vers la queue, Y sur le côté, Z vers le haut.
struct FMeshBuilder
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FLinearColor> Colors;

	// Surface fermée passant par des anneaux de même taille ; les deux bouts sont bouchés.
	void Loft(const TArray<FRing>& Rings, TFunctionRef<FLinearColor(const FVector&)> Color)
	{
		const int32 Base = Vertices.Num();
		const int32 Count = Rings[0].Num();
		FVector Middle = FVector::ZeroVector;
		for (const FRing& Ring : Rings)
		{
			for (const FVector& Point : Ring)
			{
				Vertices.Add(Point * kCmPerM);
				Normals.Add(FVector::ZeroVector);
				Colors.Add(Color(Point));
				Middle += Point;
			}
		}
		Middle /= Rings.Num() * Count;

		auto AddTriangle = [&](int32 A, int32 B, int32 C, const FVector& Outward)
		{
			// Face avant d'Unreal : (B - A) x (C - A) vers l'extérieur
			FVector Normal = FVector::CrossProduct(Vertices[B] - Vertices[A], Vertices[C] - Vertices[A]);
			if (FVector::DotProduct(Normal, Outward) < 0)
			{
				Swap(B, C);
				Normal = -Normal;
			}
			Triangles.Append({A, B, C});
			Normals[A] += Normal;
			Normals[B] += Normal;
			Normals[C] += Normal;
		};

		for (int32 R = 0; R + 1 < Rings.Num(); ++R)
		{
			FVector RingCenter = FVector::ZeroVector;
			for (int32 I = 0; I < Count; ++I)
			{
				RingCenter += Rings[R][I] + Rings[R + 1][I];
			}
			RingCenter = RingCenter / (2.0 * Count) * kCmPerM;
			for (int32 I = 0; I < Count; ++I)
			{
				const int32 Next = (I + 1) % Count;
				const int32 A = Base + R * Count + I, B = Base + R * Count + Next;
				const int32 C = Base + (R + 1) * Count + I, D = Base + (R + 1) * Count + Next;
				const FVector Outward = (Vertices[A] + Vertices[D]) / 2.0 - RingCenter;
				AddTriangle(A, B, D, Outward);
				AddTriangle(A, D, C, Outward);
			}
		}

		// Bouchons : un éventail autour du centre de chaque bout
		auto Cap = [&](int32 RingIndex, const FVector& Outward)
		{
			FVector Center = FVector::ZeroVector;
			for (const FVector& Point : Rings[RingIndex])
			{
				Center += Point;
			}
			Center /= Count;
			const int32 CenterIndex = Vertices.Add(Center * kCmPerM);
			Normals.Add(FVector::ZeroVector);
			Colors.Add(Color(Center));
			for (int32 I = 0; I < Count; ++I)
			{
				AddTriangle(CenterIndex, Base + RingIndex * Count + I, Base + RingIndex * Count + (I + 1) % Count, Outward);
			}
		};
		Cap(0, Rings[0][0] - Rings[1][0]);
		Cap(Rings.Num() - 1, Rings.Last()[0] - Rings[Rings.Num() - 2][0]);
	}

	void Apply(UProceduralMeshComponent& Mesh)
	{
		for (FVector& Normal : Normals)
		{
			Normal.Normalize();
		}
		Mesh.CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, {}, Colors, {}, false);
	}
};

// Section de fuselage : plus haute que large, le ventre et le dos peuvent différer.
struct FStation
{
	double X, Z, HalfWidth, Top, Bottom;
};

FRing Section(const FStation& S)
{
	FRing Ring;
	for (int32 I = 0; I < kRingPoints; ++I)
	{
		const double A = 2.0 * UE_DOUBLE_PI * I / kRingPoints;
		const double Sin = FMath::Sin(A);
		Ring.Add(FVector(S.X, S.HalfWidth * FMath::Cos(A), S.Z + (Sin > 0 ? S.Top : S.Bottom) * Sin));
	}
	return Ring;
}

TArray<FRing> Sections(TConstArrayView<FStation> Stations)
{
	TArray<FRing> Rings;
	for (const FStation& S : Stations)
	{
		Rings.Add(Section(S));
	}
	return Rings;
}

// Profil d'aile, bord d'attaque à LeadingX, dans le plan horizontal (aile) ou vertical (dérive).
FRing WingSection(double LeadingX, double Chord, double Thickness, const FVector& Origin, const FVector& ThicknessAxis)
{
	// Épaisseur maximale au tiers avant, bord de fuite fin
	static const double Profile[][2] = {{0.0, 0.0}, {0.05, 0.55}, {0.3, 1.0}, {0.65, 0.65}, {1.0, 0.04}};
	FRing Ring;
	for (const auto& P : Profile)
	{
		Ring.Add(Origin + FVector(LeadingX + P[0] * Chord, 0, 0) + ThicknessAxis * (P[1] * Thickness / 2.0));
	}
	for (int32 I = UE_ARRAY_COUNT(Profile) - 2; I > 0; --I)
	{
		Ring.Add(Origin + FVector(LeadingX + Profile[I][0] * Chord, 0, 0) - ThicknessAxis * (Profile[I][1] * Thickness / 2.0));
	}
	return Ring;
}

// Aile entière, de bout en bout, passant par l'emplanture.
void AddWing(FMeshBuilder& Mesh, double RootX, double RootChord, double TipX, double TipChord, double HalfSpan, double Z,
	double TipRise, double Thickness, TFunctionRef<FLinearColor(const FVector&)> Color)
{
	const FVector Up(0, 0, 1);
	Mesh.Loft({WingSection(TipX, TipChord, Thickness * 0.6, FVector(0, -HalfSpan, Z + TipRise), Up),
		WingSection(RootX, RootChord, Thickness, FVector(0, -0.5, Z), Up),
		WingSection(RootX, RootChord, Thickness, FVector(0, 0.5, Z), Up),
		WingSection(TipX, TipChord, Thickness * 0.6, FVector(0, HalfSpan, Z + TipRise), Up)}, Color);
}

void AddFin(FMeshBuilder& Mesh, double RootX, double RootChord, double RootZ, double TipX, double TipChord, double TipZ, double Thickness,
	TFunctionRef<FLinearColor(const FVector&)> Color)
{
	const FVector Side(0, 1, 0);
	Mesh.Loft({WingSection(RootX, RootChord, Thickness, FVector(0, 0, RootZ), Side),
		WingSection(TipX, TipChord, Thickness * 0.7, FVector(0, 0, TipZ), Side)}, Color);
}

void AddBar(FMeshBuilder& Mesh, const FVector& From, const FVector& To, double HalfWidth, const FLinearColor& Color)
{
	FVector Axis = (To - From).GetSafeNormal();
	FVector Side = FVector::CrossProduct(Axis, FVector(1, 0, 0));
	if (Side.IsNearlyZero())
	{
		Side = FVector(0, 1, 0);
	}
	Side = Side.GetSafeNormal() * HalfWidth;
	const FVector Other = FVector::CrossProduct(Axis, Side).GetSafeNormal() * HalfWidth;
	auto Ring = [&](const FVector& C) { return FRing{C + Side + Other, C + Side - Other, C - Side - Other, C - Side + Other}; };
	Mesh.Loft({Ring(From), Ring(To)}, [&](const FVector&) { return Color; });
}

struct FAircraftShape
{
	FMeshBuilder Body;
	TArray<FVector> PropellerHubs;
	double PropellerRadius = 2.0;
};

// Canadair CL-415 : 19,8 m de long, 28,6 m d'envergure. Coque de bateau, aile haute et droite, moteurs sur l'aile,
// flotteurs de bout d'aile, grand empennage.
FAircraftShape CanadairShape()
{
	FAircraftShape Shape;
	FMeshBuilder& M = Shape.Body;
	static const FStation Hull[] = {
		{0.0, -0.25, 0.12, 0.12, 0.12}, {0.7, -0.15, 0.7, 0.65, 0.7}, {2.4, 0.1, 1.15, 1.2, 1.3}, {3.6, 0.15, 1.25, 1.3, 1.45},
		{6.0, 0.15, 1.3, 1.3, 1.55}, {10.0, 0.2, 1.3, 1.25, 1.5}, {13.0, 0.5, 1.05, 1.0, 1.05}, {16.0, 1.05, 0.65, 0.65, 0.5},
		{19.8, 1.7, 0.1, 0.15, 0.1}};
	M.Loft(Sections(Hull), [](const FVector& P)
	{
		return P.Z < -0.45 + P.X * 0.05 ? kRed : (P.X > 13.8 && P.X < 15.0 ? kRed : kYellow);
	});
	// Pare-brise, un peu en saillie
	M.Loft({Section({1.3, 0.72, 0.8, 0.42, 0.3}), Section({2.6, 1.0, 1.0, 0.42, 0.3})}, [](const FVector&) { return kDark; });

	AddWing(M, 7.0, 3.6, 7.3, 3.0, 14.3, 1.55, 0.0, 0.5, [](const FVector& P)
	{
		return FMath::Abs(P.Y) > 11.5 ? kRed : kYellow;
	});
	for (const double Side : {-1.0, 1.0})
	{
		// Moteur, posé sur l'aile
		const double Y = Side * 4.6;
		static const FStation Nacelle[] = {{4.9, 1.4, 0.2, 0.2, 0.2}, {5.5, 1.4, 0.6, 0.65, 0.6}, {8.5, 1.5, 0.62, 0.72, 0.5}, {11.5, 1.65, 0.1, 0.1, 0.1}};
		TArray<FRing> Rings = Sections(Nacelle);
		for (FRing& Ring : Rings)
		{
			for (FVector& P : Ring)
			{
				P.Y += Y;
			}
		}
		M.Loft(Rings, [](const FVector& P) { return P.X < 5.8 ? kRed : kYellow; });
		Shape.PropellerHubs.Add(FVector(4.8, Y, 1.4));

		// Flotteur de bout d'aile
		const double FloatY = Side * 12.7;
		static const FStation Float[] = {{7.2, -0.75, 0.08, 0.08, 0.08}, {8.0, -0.8, 0.34, 0.3, 0.34}, {9.6, -0.8, 0.34, 0.3, 0.34}, {10.8, -0.72, 0.06, 0.06, 0.06}};
		TArray<FRing> FloatRings = Sections(Float);
		for (FRing& Ring : FloatRings)
		{
			for (FVector& P : Ring)
			{
				P.Y += FloatY;
			}
		}
		M.Loft(FloatRings, [](const FVector&) { return kRed; });
		AddBar(M, FVector(8.8, FloatY, -0.6), FVector(8.8, FloatY, 1.45), 0.09, kGrey);
	}

	AddFin(M, 14.2, 4.6, 1.6, 17.2, 2.2, 7.2, 0.32, [](const FVector& P) { return P.Z > 5.6 ? kRed : kYellow; });
	AddWing(M, 16.6, 2.5, 17.6, 1.4, 5.2, 5.0, 0.0, 0.2, [](const FVector&) { return kYellow; });
	Shape.PropellerRadius = 2.0;
	return Shape;
}

// Dash 8 Q400MR : 32,8 m de long, 28,4 m d'envergure. Long et fin, aile haute, longs moteurs sous l'aile, empennage en T,
// réservoir de retardant sous le ventre.
FAircraftShape DashShape()
{
	FAircraftShape Shape;
	FMeshBuilder& M = Shape.Body;
	static const FStation Fuselage[] = {
		{0.0, 0.0, 0.1, 0.1, 0.1}, {1.2, 0.05, 0.8, 0.8, 0.75}, {3.8, 0.1, 1.25, 1.25, 1.2}, {6.0, 0.1, 1.35, 1.35, 1.35},
		{22.0, 0.1, 1.35, 1.35, 1.35}, {26.0, 0.45, 1.1, 1.05, 0.95}, {30.0, 1.0, 0.6, 0.6, 0.38}, {32.8, 1.4, 0.1, 0.12, 0.08}};
	M.Loft(Sections(Fuselage), [](const FVector& P) { return P.Z < -0.15 + (P.X > 22.0 ? (P.X - 22.0) * 0.1 : 0.0) ? kRed : kYellow; });
	M.Loft({Section({1.6, 0.62, 0.86, 0.4, 0.3}), Section({3.2, 0.9, 1.08, 0.42, 0.3})}, [](const FVector&) { return kDark; });
	// Réservoir de largage sous le ventre
	static const FStation Tank[] = {{8.5, -1.25, 0.2, 0.2, 0.2}, {10.0, -1.35, 0.9, 0.45, 0.55}, {20.0, -1.35, 0.9, 0.45, 0.55}, {22.0, -1.2, 0.2, 0.2, 0.2}};
	M.Loft(Sections(Tank), [](const FVector&) { return kRed; });

	AddWing(M, 12.3, 3.3, 13.8, 1.3, 14.2, 1.55, 0.25, 0.48, [](const FVector& P)
	{
		return FMath::Abs(P.Y) > 12.0 ? kRed : kYellow;
	});
	for (const double Side : {-1.0, 1.0})
	{
		const double Y = Side * 4.2;
		static const FStation Nacelle[] = {{9.4, 1.05, 0.2, 0.2, 0.2}, {10.0, 1.05, 0.66, 0.66, 0.66}, {14.0, 1.0, 0.68, 0.7, 0.7}, {18.5, 1.05, 0.12, 0.12, 0.12}};
		TArray<FRing> Rings = Sections(Nacelle);
		for (FRing& Ring : Rings)
		{
			for (FVector& P : Ring)
			{
				P.Y += Y;
			}
		}
		M.Loft(Rings, [](const FVector& P) { return P.X < 10.4 ? kRed : kYellow; });
		Shape.PropellerHubs.Add(FVector(9.3, Y, 1.05));
	}

	AddFin(M, 26.5, 4.8, 1.2, 30.4, 2.2, 6.7, 0.32, [](const FVector& P) { return P.Z > 5.0 ? kRed : kYellow; });
	AddWing(M, 30.2, 2.3, 31.3, 1.2, 4.8, 6.75, 0.0, 0.2, [](const FVector&) { return kYellow; });
	Shape.PropellerRadius = 2.05;
	return Shape;
}

FMeshBuilder PropellerShape(double Radius)
{
	FMeshBuilder M;
	constexpr int32 kBlades = 6;
	for (int32 I = 0; I < kBlades; ++I)
	{
		const double A = 2.0 * UE_DOUBLE_PI * I / kBlades;
		const FVector Tip(0, Radius * FMath::Cos(A), Radius * FMath::Sin(A));
		AddBar(M, Tip * 0.1, Tip, 0.09, kDark);
	}
	static const FStation Spinner[] = {{-0.45, 0.0, 0.03, 0.03, 0.03}, {-0.15, 0.0, 0.3, 0.3, 0.3}, {0.2, 0.0, 0.34, 0.34, 0.34}};
	M.Loft(Sections(Spinner), [](const FVector&) { return kGrey; });
	return M;
}
}  // namespace

AFireAircraft::AFireAircraft()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Body = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AFireAircraft::Build(bool bDash, UMaterialInterface* Material)
{
	FAircraftShape Shape = bDash ? DashShape() : CanadairShape();
	Shape.Body.Apply(*Body);
	Body->SetMaterial(0, Material);
	// Les calques du feu projetés sur le terrain traversent tout ce qui vole au-dessus : ils coloreraient l'avion
	Body->SetReceivesDecals(false);
	// Le modèle part du nez, vers la queue : on le retourne, nez vers l'avant de l'acteur, centré sur le milieu de l'avion
	const double CenterX = bDash ? 15.0 : 9.0;
	Body->SetRelativeLocationAndRotation(FVector(CenterX * kCmPerM, 0, 0), FRotator(0.0, 180.0, 0.0));

	FMeshBuilder Blades = PropellerShape(Shape.PropellerRadius);
	for (const FVector& Hub : Shape.PropellerHubs)
	{
		UProceduralMeshComponent* Propeller = NewObject<UProceduralMeshComponent>(this);
		Propeller->SetupAttachment(Body);
		Propeller->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Propeller->SetReceivesDecals(false);
		Propeller->RegisterComponent();
		Propeller->SetRelativeLocation(Hub * kCmPerM);
		Blades.Apply(*Propeller);
		Propeller->SetMaterial(0, Material);
		Propellers.Add(Propeller);
	}
}

void AFireAircraft::SpinPropellers(float DeltaSeconds, bool bRunning)
{
	if (!bRunning)
	{
		return;
	}
	PropellerAngle = FMath::Fmod(PropellerAngle + DeltaSeconds * 1500.0, 360.0);
	for (UProceduralMeshComponent* Propeller : Propellers)
	{
		Propeller->SetRelativeRotation(FRotator(0.0, 0.0, PropellerAngle));
	}
}

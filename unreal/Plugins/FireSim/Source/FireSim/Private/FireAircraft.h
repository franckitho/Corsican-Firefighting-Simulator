#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FireAircraft.generated.h"

class UMaterialInterface;
class UProceduralMeshComponent;

// Petit avion de lutte : Canadair CL-415 ou Dash 8 Q400MR, modelé par le code (aucun fichier 3D à importer), aux
// proportions réelles et aux couleurs de la Sécurité civile. Le simulateur le place à chaque image.
UCLASS(NotPlaceable, Transient)
class AFireAircraft : public AActor
{
	GENERATED_BODY()

public:
	AFireAircraft();

	void Build(bool bDash, UMaterialInterface* Material);
	void SpinPropellers(float DeltaSeconds, bool bRunning);

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> Root;

	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Body;

	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> Propellers;

	double PropellerAngle = 0;
};

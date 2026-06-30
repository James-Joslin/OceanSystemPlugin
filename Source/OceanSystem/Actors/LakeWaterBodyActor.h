// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeWaterBodyActor.generated.h"

class UOceanBodyComponent;
class UTiledWaterMeshComponent;

UCLASS(meta = (DisplayName = "Lake Water Body"))
class OCEANSYSTEM_API ALakeWaterBodyActor : public AActor
{
	GENERATED_BODY()

public:
	ALakeWaterBodyActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lake")
	TObjectPtr<UOceanBodyComponent> OceanBody;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lake")
	TObjectPtr<UTiledWaterMeshComponent> TiledMesh;

	UFUNCTION(BlueprintCallable, Category = "Lake")
	void RefreshMeshMaterial();

	UFUNCTION(BlueprintCallable, Category = "Lake")
	void UpdateBoundsFromWaveConfig();

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
#endif
};
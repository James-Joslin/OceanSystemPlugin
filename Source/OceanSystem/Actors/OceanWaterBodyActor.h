// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OceanWaterBodyActor.generated.h"

class UOceanBodyComponent;
class UTiledWaterMeshComponent;

UCLASS(meta = (DisplayName = "Ocean Water Body"))
class OCEANSYSTEM_API AOceanWaterBodyActor : public AActor
{
	GENERATED_BODY()

public:
	AOceanWaterBodyActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ocean")
	TObjectPtr<UOceanBodyComponent> OceanBody;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ocean")
	TObjectPtr<UTiledWaterMeshComponent> TiledMesh;

	UFUNCTION(BlueprintCallable, Category = "Ocean")
	void RefreshMeshMaterial();

	UFUNCTION(BlueprintCallable, Category = "Ocean")
	void UpdateBoundsFromWaveConfig();

protected:
	virtual void BeginPlay() override;

	/** Tick in editor so tile LODs respond to the editor viewport camera. */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
#endif
};
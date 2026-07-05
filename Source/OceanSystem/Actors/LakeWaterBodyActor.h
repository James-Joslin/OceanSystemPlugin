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

	UFUNCTION(BlueprintCallable, Category = "Lake")
	void SyncExtentFromMesh();

	/** Snap the tiled mesh back to the root transform if it has drifted.
		The rendered surface and the registered physics surface must share
		a transform — reposition water by moving the actor, not the mesh. */
	void EnforceMeshTransformLock();

protected:
	virtual void BeginPlay() override;

	/** Tick in editor so tile LODs respond to the editor viewport camera. */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
#endif
};
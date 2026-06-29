// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeWaterBodyActor.generated.h"

class UOceanBodyComponent;
class UTiledWaterMeshComponent;
// Step 16: class UUnderwaterPostProcessComponent;

/**
 * Drop-in lake actor. Place in level at the desired water surface Z.
 *
 * Component hierarchy:
 *   Root: UOceanBodyComponent  (BodyType=Lake, Priority=10)
 *     └── UTiledWaterMeshComponent  (TilesX × TilesY with 5-level LOD)
 *     └── (Step 16) UUnderwaterPostProcessComponent
 *
 * Shares M_OceanBase with the ocean — different wave parameters give a
 * calmer surface. Set BaseMaterial on the OceanBody component to the
 * same material; the MID receives this lake's own FWaveConfig.
 *
 * Position the actor's Z at the lake surface height. The subsystem
 * uses this Z as the BaseZ for wave evaluation and buoyancy queries.
 */
UCLASS(meta = (DisplayName = "Lake Water Body"))
class OCEANSYSTEM_API ALakeWaterBodyActor : public AActor
{
	GENERATED_BODY()

public:
	ALakeWaterBodyActor();

	// -------------------------------------------------------------------
	// Components
	// -------------------------------------------------------------------

	/** Water body config, MID ownership, and subsystem registration. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lake")
	TObjectPtr<UOceanBodyComponent> OceanBody;

	/** Tiled LOD mesh grid for the lake surface. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lake")
	TObjectPtr<UTiledWaterMeshComponent> TiledMesh;

	// Step 16: UnderwaterPostProcessComponent

	// -------------------------------------------------------------------
	// Runtime API
	// -------------------------------------------------------------------

	/** Reapply the MID to all tile mesh sections. */
	UFUNCTION(BlueprintCallable, Category = "Lake")
	void RefreshMeshMaterial();

	/** Recompute tile VerticalBoundsExtension from the wave config. */
	UFUNCTION(BlueprintCallable, Category = "Lake")
	void UpdateBoundsFromWaveConfig();

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
#endif
};
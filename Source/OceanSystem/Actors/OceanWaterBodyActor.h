// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OceanWaterBodyActor.generated.h"

class UOceanBodyComponent;
class UClipmapOceanMeshComponent;
// Step 16: class UUnderwaterPostProcessComponent;
// Step 18: class UOceanSprayComponent;

/**
 * Drop-in ocean actor. Place in level → animated Gerstner ocean with foam.
 *
 * Component hierarchy:
 *   Root: UOceanBodyComponent  (BodyType=Ocean, Priority=0)
 *     └── UClipmapOceanMeshComponent  (camera-snapping concentric LOD rings)
 *     └── (Step 16) UUnderwaterPostProcessComponent
 *     └── (Step 18) OceanSprayComponent
 *
 * On BeginPlay the actor wires the body component's Dynamic Material Instance
 * to all clipmap mesh sections and computes VerticalBoundsExtension from the
 * wave config's maximum displacement. The subsystem ticks MID parameters
 * (wave time + layer data) automatically.
 *
 * Setup:
 *   1. Set BaseMaterial on the OceanBody component to a material that
 *      includes GerstnerWave.ush and reads Wave_N / WaveDir_N parameters.
 *   2. Optionally tune WaveConfig, RingCount, BaseCellSize, etc.
 *   3. Play → ocean animates.
 */
UCLASS(meta = (DisplayName = "Ocean Water Body"))
class OCEANSYSTEM_API AOceanWaterBodyActor : public AActor
{
	GENERATED_BODY()

public:
	AOceanWaterBodyActor();

	// -------------------------------------------------------------------
	// Components — set in constructor, visible in Details panel
	// -------------------------------------------------------------------

	/** Water body config, MID ownership, and subsystem registration. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ocean")
	TObjectPtr<UOceanBodyComponent> OceanBody;

	/** Camera-centred concentric LOD ring mesh. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ocean")
	TObjectPtr<UClipmapOceanMeshComponent> ClipmapMesh;

	// Step 16: UnderwaterPostProcessComponent
	// Step 18: OceanSprayComponent

	// -------------------------------------------------------------------
	// Runtime API
	// -------------------------------------------------------------------

	/**
	 * Reapply the MID to all mesh sections.
	 * Called automatically on BeginPlay; call manually if the base material
	 * or mesh is rebuilt at runtime.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ocean")
	void RefreshMeshMaterial();

	/**
	 * Recompute VerticalBoundsExtension from the wave config.
	 * Called automatically on BeginPlay and after wave config changes.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ocean")
	void UpdateBoundsFromWaveConfig();

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
#endif
};
// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "../Types/OceanTypes.h"
#include "WaterBodyComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UWaveParameterSubsystem;

/**
 * Core component for a water body. Owns the wave configuration and
 * Dynamic Material Instance for a single ocean, lake, or river.
 *
 * Lifecycle:
 *   BeginPlay  — creates MID from BaseMaterial, builds an FWaterBodyEntry,
 *                registers with WaveParameterSubsystem (auto-blend detection,
 *                layer sorting), and pushes initial wave params.
 *   EndPlay    — unregisters from subsystem.
 *
 * Attach to an actor alongside the appropriate geometry component
 * (ClipmapOceanMesh, TiledWaterMesh, or SplineMeshComponents).
 */
UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UWaterBodyComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UWaterBodyComponent();

	// -------------------------------------------------------------------
	// Properties
	// -------------------------------------------------------------------

	/** Water body type — controls geometry strategy and query behaviour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body")
	EOceanBodyType BodyType = EOceanBodyType::Ocean;

	/** Wave parameters for this body. Layers are sorted by amplitude on registration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Waves")
	FWaveConfig WaveConfig;

	/**
	 * XY half-extents in world units (ocean/lake only).
	 * Defines the spatial footprint for queries and blend zone detection.
	 * Ignored for rivers (spline + half-width is used instead).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Bounds",
		meta = (EditCondition = "BodyType != EOceanBodyType::River"))
	FVector2D Extent = FVector2D(10000.0, 10000.0);

	/** Overlap resolution priority. Higher wins spatial queries. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body")
	int32 Priority = 0;

	/**
	 * Base material to create the Dynamic Material Instance from.
	 * Must contain the Custom node that includes GerstnerWave.ush and
	 * reads the Wave_N / WaveDir_N vector parameters.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Material")
	TSoftObjectPtr<UMaterialInterface> BaseMaterial;

	/** Default blend zone width when this body overlaps another. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Blending",
		meta = (ClampMin = "0.0", UIMin = "50.0", UIMax = "1000.0"))
	float BlendWidth = 200.0f;

	// -------------------------------------------------------------------
	// Runtime access
	// -------------------------------------------------------------------

	/** The Dynamic Material Instance created at BeginPlay. */
	UFUNCTION(BlueprintCallable, Category = "Water Body")
	UMaterialInstanceDynamic* GetMaterialInstance() const { return MaterialInstance; }

	/**
	 * Update the wave config at runtime. Sorts layers by amplitude,
	 * marks the body dirty for MID resync, and notifies the subsystem.
	 */
	UFUNCTION(BlueprintCallable, Category = "Water Body|Waves")
	void SetWaveConfig(const FWaveConfig& NewConfig);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Build an FWaterBodyEntry from current properties for subsystem registration. */
	FWaterBodyEntry BuildRegistryEntry() const;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> MaterialInstance = nullptr;
};
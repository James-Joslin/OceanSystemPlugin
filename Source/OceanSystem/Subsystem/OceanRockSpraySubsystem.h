// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "OceanRockSpraySubsystem.generated.h"

class UOceanVfxVariantSet;

/**
 * Rock impact spray (Phase D2).
 *
 * No per-rock components and no collision detection. Spray points are
 * authored ONCE per mesh asset as static mesh sockets ("Spray_*", X-axis
 * = outward face normal); this subsystem discovers every placed instance
 * of those meshes at world begin-play — plain StaticMeshComponents and
 * PCG-placed Instanced/HISM components alike — composes instance x socket
 * transforms into a flat world-space point list, and pre-filters out
 * points nowhere near the water (cliff-top sockets never survive).
 *
 * Per tick, points within ActiveRadius of the camera are evaluated as
 * pure queries against the WaveParameterSubsystem:
 *
 *   FIRE when the surface crosses UPWARD past the point's Z while wave
 *   velocity opposes the socket normal (water moving INTO the face).
 *   Intensity scales with crossing speed — big swells detonate, ripples
 *   below MinCrossingSpeed fizzle silently.
 *
 * Bursts are requested from UOceanVfxSubsystem, which owns pooling,
 * per-site cooldowns, the per-frame cap, and distance culling.
 *
 * Configuration lives in UOceanVfxSettings (Project Settings -> Plugins
 * -> Ocean VFX). No RockImpactVariantSet assigned = subsystem idles.
 *
 * Debug console commands (see .cpp):
 *   Ocean.Vfx.Rocks.Rescan
 *   Ocean.Vfx.Rocks.Show [Seconds]
 *   Ocean.Vfx.Rocks.Stats
 */
UCLASS()
class OCEANSYSTEM_API UOceanRockSpraySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// --- UTickableWorldSubsystem ---
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

	/**
	 * Rebuild the spray point list from the current world. Called
	 * automatically at world begin-play; call again if PCG regenerates
	 * rocks at runtime.
	 */
	void RescanSprayPoints();

	/** Number of spray points that survived the scan pre-filter. */
	int32 GetPointCount() const { return Points.Num(); }

private:
	/** One world-space spray point (instance x socket composition). */
	struct FSprayPoint
	{
		FVector Location = FVector::ZeroVector;

		/** Outward face normal (socket X-axis, normalised). */
		FVector Normal = FVector::ForwardVector;

		/** From socket relative scale X — hero-crag multiplier. */
		float IntensityMul = 1.0f;

		// --- Per-tick trigger state ---
		float PrevSurfaceZ = 0.0f;
		double LastEvalTime = 0.0;
		bool bHavePrev = false;
	};

	TArray<FSprayPoint> Points;

	/** XY spatial grid: cell -> indices into Points. Cheap camera-radius
		gathering without touching the whole list. */
	TMap<FIntPoint, TArray<int32>> Grid;

	/** Hard reference keeps the settings-assigned variant set loaded. */
	UPROPERTY(Transient)
	TObjectPtr<UOceanVfxVariantSet> VariantSet;

	bool bScanned = false;

	void GatherPointsInRadius(
		const FVector& Center, float Radius,
		TArray<int32>& OutIndices) const;
	static FIntPoint MakeCell(const FVector& Location);

	friend class FOceanRockSprayDebugCommands;
};
// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "../Types/OceanTypes.h"
#include "../WaveEval/GerstnerEvaluator.h"
#include "WaveParameterSubsystem.generated.h"

class UTexture2D;
class UOceanBodyComponent;

// ---------------------------------------------------------------------------
// FWaterBodyQueryResult
// ---------------------------------------------------------------------------

/**
 * Result of a spatial query against the water body registry.
 * Used internally by the subsystem to dispatch to the correct evaluator.
 */
struct FWaterBodyQueryResult
{
	/** Index of the primary (highest-priority) body containing the query point. */
	int32 PrimaryIndex = INDEX_NONE;

	/** Index of the secondary body for blend zone interpolation. INDEX_NONE if not blending. */
	int32 SecondaryIndex = INDEX_NONE;

	/** Blend alpha: 0 = pure primary, 1 = pure secondary. Only valid when SecondaryIndex != INDEX_NONE. */
	float BlendAlpha = 0.0f;

	bool IsValid() const { return PrimaryIndex != INDEX_NONE; }
	bool IsBlending() const { return SecondaryIndex != INDEX_NONE; }
};

// ---------------------------------------------------------------------------
// UWaveParameterSubsystem
// ---------------------------------------------------------------------------

/**
 * World subsystem that manages all water bodies in the level.
 *
 * Responsibilities:
 *   1. Water body registry with priority-sorted entries.
 *   2. Blend zone auto-detection on registration.
 *   3. Spatial queries to find which body covers a world position.
 *   4. CPU evaluation dispatch � physics path (buoyancy) and full path
 *      (spray, PP, debug), including river spline and blend zone variants.
 *   5. Per-frame MID sync: pushes wave parameters and scaled time to each
 *      body's Dynamic Material Instance.
 *   6. Layer sort enforcement on registration and config update.
 *
 * Ticks in editor to drive wave animation on the material via
 * FApp::GetCurrentTime(). At runtime uses World->GetTimeSeconds()
 * which respects pause and time dilation.
 *
 * No MPC � everything is per-body via Dynamic Material Instances.
 */
UCLASS()
class OCEANSYSTEM_API UWaveParameterSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// -------------------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------------------

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Tick in editor so wave time drives the material preview. */
	virtual bool IsTickableInEditor() const override { return true; }

	// -------------------------------------------------------------------
	// Water Body Registry
	// -------------------------------------------------------------------

	/**
	 * Register a water body. The entry should be fully populated by the
	 * calling OceanBodyComponent. Layers are re-sorted by amplitude on
	 * registration. Blend zones are auto-detected against existing bodies.
	 * The registry is re-sorted by priority descending.
	 */
	void RegisterWaterBody(const FWaterBodyEntry& Entry);

	/**
	 * Remove a water body from the registry. Cleans up any blend zones
	 * referencing the body.
	 */
	void UnregisterWaterBody(const UOceanBodyComponent* Body);

	/**
	 * Update a body's wave config. Re-sorts layers by amplitude, marks
	 * the body dirty for MID resync, and refreshes blend zones.
	 */
	void UpdateWaterBodyConfig(const UOceanBodyComponent* Body, const FWaveConfig& NewConfig);

	/**
	 * Mark a body dirty so its MID is resynced on the next tick.
	 */
	void MarkBodyDirty(const UOceanBodyComponent* Body);

	// -------------------------------------------------------------------
	// Spatial Queries
	// -------------------------------------------------------------------

	/**
	 * Find the primary water body at a world XY position.
	 * Returns the highest-priority body whose bounds contain the point,
	 * or nullptr if no body covers that position.
	 */
	const FWaterBodyEntry* FindWaterBodyAt(const FVector2D& XY) const;

	// -------------------------------------------------------------------
	// CPU Evaluation � Physics Path (buoyancy)
	// -------------------------------------------------------------------

	/**
	 * Get the physics-LOD water surface height at a world position.
	 * Evaluates only PhysicsLayerCount layers. Handles rivers (spline Z)
	 * and blend zones (interpolated configs) automatically.
	 *
	 * @return true if the position is over a water body.
	 */
	bool GetWaveHeight(const FVector& WorldPos, float& OutWorldZ);

	/**
	 * Get the full physics-LOD evaluation result at a world position.
	 * Same dispatch as GetWaveHeight but returns the complete FGerstnerResult.
	 *
	 * @return true if the position is over a water body.
	 */
	bool GetWaveData(const FVector& WorldPos, FGerstnerResult& OutResult);

	// -------------------------------------------------------------------
	// CPU Evaluation � Full Path (spray, PP, debug)
	// -------------------------------------------------------------------

	/**
	 * Get the full-layer water surface height at a world position.
	 * Evaluates ALL visual layers. Use for spray detection, underwater PP,
	 * and debug visualisation.
	 *
	 * @return true if the position is over a water body.
	 */
	bool GetFullWaveHeight(const FVector& WorldPos, float& OutWorldZ);

	/**
	 * Get the Jacobian fold intensity at a world position.
	 * Evaluates ALL visual layers. Use for spray crest detection.
	 *
	 * @return Fold intensity [0,1], or 0.0 if no body covers the position.
	 */
	float GetFoldIntensity(const FVector& WorldPos);

	// -------------------------------------------------------------------
	// Read-only access
	// -------------------------------------------------------------------

	/** Number of registered water bodies. */
	int32 GetWaterBodyCount() const { return WaterBodies.Num(); }

	/** Read-only access to the registry. */
	const TArray<FWaterBodyEntry>& GetWaterBodies() const { return WaterBodies; }

private:
	// -------------------------------------------------------------------
	// Internal
	// -------------------------------------------------------------------

	/** Find the index of a body by its owner pointer. INDEX_NONE if not found. */
	int32 FindEntryIndex(const UOceanBodyComponent* Body) const;

	/** Re-sort registry by priority descending. */
	void SortRegistryByPriority();

	/** Test whether XY is inside a body's footprint (bounds or spline+halfwidth). */
	bool IsPointInBody(const FWaterBodyEntry& Entry, const FVector2D& XY) const;

	/**
	 * Distance from a body's edge boundary at an XY position.
	 * Positive = inside, negative = outside. Used for blend alpha.
	 * Ocean returns FLT_MAX (no edge). River uses spline distance.
	 */
	float DistanceFromBodyEdge(const FWaterBodyEntry& Entry, const FVector2D& XY) const;

	/** Detect and create blend zones between a newly registered body and all existing ones. */
	void DetectBlendZones(int32 NewBodyIndex);

	/** Remove all blend zone entries referencing a body. */
	void RemoveBlendZonesFor(const UOceanBodyComponent* Body);

	/**
	 * Full spatial query: find primary body, optional secondary for blend,
	 * and compute blend alpha.
	 */
	FWaterBodyQueryResult QueryBodiesAt(const FVector& WorldPos) const;

	/**
	 * Internal dispatch: evaluate a single body at the given world position.
	 * Selects flat vs spline evaluator based on body type.
	 *
	 * @param bFullEval  true = all layers, false = physics LOD.
	 */
	FGerstnerResult EvaluateBody(
		const FWaterBodyEntry& Entry, const FVector& WorldPos,
		float WorldTime, bool bFullEval) const;

	/**
	 * Push all wave parameters and current time into a body's MID.
	 * Called on tick for dirty bodies (full param sync) and every frame
	 * (time update only).
	 */
	void SyncMaterialInstance(FWaterBodyEntry& Entry, float WorldTime);

	/**
	* Create a transient 128�2 RGBA32F texture for wave data.
	* No disk asset � lives in memory only, GC'd when unreferenced.
	*/
	UTexture2D* CreateWaveDataTexture() const;

	/**
	 * Write wave layer data into an existing data texture.
	 * Row 0: (Amplitude, Wavelength, Steepness, Speed)
	 * Row 1: (Direction.X, Direction.Y, PhaseOffset, 0)
	 * Unused slots are zeroed.
	 */
	void UpdateWaveDataTexture(UTexture2D* Texture, const FWaveConfig& Config) const;

	// -------------------------------------------------------------------
	// Data
	// -------------------------------------------------------------------

	/** All registered water bodies, sorted by priority descending. */
	TArray<FWaterBodyEntry> WaterBodies;
};
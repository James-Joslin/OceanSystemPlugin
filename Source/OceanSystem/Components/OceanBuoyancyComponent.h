// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "OceanBuoyancyComponent.generated.h"

class UPrimitiveComponent;
class UWaveParameterSubsystem;

/**
 * Point-based buoyancy using per-point spring-dampers and the
 * physics-LOD wave evaluation path.
 *
 * Model (per sample point, per tick):
 *
 *   Depth   = WaterZ - PointZ                       (clamped to MaxSubmersionDepth)
 *   Spring  = k * Depth                              k derived from body mass
 *   Damper  = -PointDamping * PointVelocity.Z * m/N  m/N = mass share per point
 *   Force   = Spring + Damper, applied AT the point via AddForceAtLocation
 *
 * The spring constant k is derived automatically from the rigid body's
 * mass so that when every point sits EquilibriumDepth below the surface,
 * total buoyancy exactly equals weight. No manual force tuning: change
 * the mesh or the mass and the object still floats correctly.
 *
 * Stability comes from applying the DAMPER at each point using that
 * point's own world velocity (GetPhysicsLinearVelocityAtPoint). A rolling
 * body moves its points vertically even when its centre is still, so
 * per-point damping resists pitch/roll directly — the torque the old
 * central damping could never produce. Depth-proportional springs then
 * provide the righting moment: the low side is deeper, so it pushes
 * harder.
 *
 * Only PhysicsLayerCount wave layers are evaluated per sample — the
 * object rides the broad swell and ignores detail chop.
 *
 * Use GenerateBoxSamplePoints (CallInEditor) to lay out a grid of points
 * on the bottom face of a bounding box, or manually populate SamplePoints.
 */
UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UOceanBuoyancyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UOceanBuoyancyComponent();

	// -------------------------------------------------------------------
	// Properties — Buoyancy
	// -------------------------------------------------------------------

	/**
	 * Sample point positions in the owning actor's local space.
	 * Each submerged point applies force at its world location,
	 * naturally producing torques when the body tilts.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy")
	TArray<FVector> SamplePoints;

	/**
	 * Depth in cm at which the object floats at rest (its draft).
	 * The spring constant is derived from mass so that total buoyancy
	 * equals weight when every point is this deep. Smaller = floats
	 * higher and stiffer; larger = sits deeper and softer.
	 * For a 100 cm box, 40-60 is typical.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy",
		meta = (ClampMin = "1.0", UIMin = "10.0", UIMax = "500.0"))
	float EquilibriumDepth = 50.0f;

	/**
	 * Depth in cm beyond which the spring force stops growing.
	 * Caps the maximum upward force at (MaxSubmersionDepth /
	 * EquilibriumDepth) x weight, so a fully dunked object surfaces
	 * briskly but never launches. 2-4x EquilibriumDepth works well.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy",
		meta = (ClampMin = "1.0", UIMin = "50.0", UIMax = "1000.0"))
	float MaxSubmersionDepth = 150.0f;

	/**
	 * Global multiplier on the derived buoyancy. 1.0 = physically
	 * balanced against mass. >1 floats higher, <1 sits deeper.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy",
		meta = (ClampMin = "0.1", UIMin = "0.5", UIMax = "3.0"))
	float BuoyancyMultiplier = 1.0f;

	// -------------------------------------------------------------------
	// Properties — Damping & Drag
	// -------------------------------------------------------------------

	/**
	 * Per-point vertical damping (1/s). Applied at each submerged point
	 * against that point's own vertical velocity. This is the primary
	 * roll/pitch stabiliser — it converts rocking motion into drag at
	 * the exact locations doing the rocking. 3-8 is a good range;
	 * higher settles faster but can look syrupy.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Damping",
		meta = (ClampMin = "0.0", UIMin = "1.0", UIMax = "15.0"))
	float PointDamping = 5.0f;

	/**
	 * Linear drag coefficient in water (mass-independent acceleration).
	 * Scales with the fraction of sample points submerged.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Damping",
		meta = (ClampMin = "0.0", UIMin = "0.01", UIMax = "2.0"))
	float WaterDragLinear = 0.3f;

	/**
	 * Angular drag coefficient in water (mass-independent, radians).
	 * Scales with the fraction of sample points submerged. Secondary
	 * to PointDamping for stability; mostly kills residual yaw spin.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Damping",
		meta = (ClampMin = "0.0", UIMin = "0.05", UIMax = "4.0"))
	float WaterDragAngular = 0.5f;

	// -------------------------------------------------------------------
	// Properties — Flags
	// -------------------------------------------------------------------

	/**
	 * Placeholder for future sailing / wind system integration.
	 * When true, indicates this body can be player-controlled on the water.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy")
	bool bIsPossessable = false;

	/** True when any sample point is below the water surface. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Buoyancy|State")
	bool bIsInWater = false;

	/** Fraction of sample points currently submerged [0,1]. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Buoyancy|State")
	float SubmergedFraction = 0.0f;

	// -------------------------------------------------------------------
	// Sample Point Generator
	// -------------------------------------------------------------------

	/** Half-extents of the bounding box for sample point generation. */
	UPROPERTY(EditAnywhere, Category = "Buoyancy|Generator")
	FVector BoxHalfExtents = FVector(50.0f, 50.0f, 50.0f);

	/** Points per axis for the generated bottom-face grid (N x N). */
	UPROPERTY(EditAnywhere, Category = "Buoyancy|Generator",
		meta = (ClampMin = "2", ClampMax = "8"))
	int32 BoxPointsPerAxis = 3;

	/** Generate an N x N grid of points on the bottom face of the box. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Buoyancy|Generator")
	void GenerateBoxSamplePoints();

	// -------------------------------------------------------------------
	// Debug
	// -------------------------------------------------------------------

	/** Draw sample points, water surface links, and a diagnostic readout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Debug")
	bool bShowDebug = false;

	/** Radius of the debug point spheres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Debug",
		meta = (ClampMin = "1.0", UIMin = "2.0", UIMax = "50.0"))
	float DebugPointRadius = 8.0f;

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Cached wave subsystem (set in BeginPlay). */
	UPROPERTY(Transient)
	TObjectPtr<UWaveParameterSubsystem> CachedSubsystem = nullptr;

	/** Cached physics body (owner's root primitive). */
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> PhysicsBody = nullptr;
};
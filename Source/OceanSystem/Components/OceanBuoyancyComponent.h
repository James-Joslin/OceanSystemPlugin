// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "OceanBuoyancyComponent.generated.h"

class UPrimitiveComponent;
class UWaveParameterSubsystem;

/**
 * Point-based buoyancy using the physics-LOD wave evaluation path.
 *
 * Attach to any actor whose root component is a UPrimitiveComponent with
 * Simulate Physics enabled. Each tick, sample points are transformed to
 * world space and queried against the wave subsystem. Submerged points
 * receive upward forces; drag and vertical damping stabilise the motion.
 *
 * Only PhysicsLayerCount wave layers are evaluated per sample — detail
 * ripples are skipped. This matches the Sea of Thieves approach: the
 * object responds to the broad ocean swell, not every small chop.
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
	 * Each submerged point applies an upward force at its world location,
	 * naturally producing torques when the body tilts.
	 *
	 * Default: 4 bottom corners + centre of a unit box.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy")
	TArray<FVector> SamplePoints;

	/**
	 * Upward force magnitude per submerged sample point in Newtons.
	 * Tune relative to the object's mass: total buoyancy across all
	 * fully-submerged points should slightly exceed the object's weight
	 * for stable floating. Too high → object launches, too low → sinks.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy",
		meta = (ClampMin = "0.0", UIMin = "100.0", UIMax = "50000.0"))
	float BuoyancyForce = 2000.0f;

	/**
	 * Exponent applied to the submersion depth before scaling the force.
	 *   1.0 = linear (force proportional to depth)
	 *   <1  = rapid initial response, levels off (stable but bouncy)
	 *   >1  = slow initial response, ramps up (less bouncy, can sink deeper)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy",
		meta = (ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0"))
	float SubmersionForceExponent = 1.0f;

	// -------------------------------------------------------------------
	// Properties — Damping & Drag
	// -------------------------------------------------------------------

	/**
	 * Vertical velocity damping (applied as acceleration, mass-independent).
	 * Opposes vertical oscillation. Higher = settles faster.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Damping",
		meta = (ClampMin = "0.0", UIMin = "0.1", UIMax = "5.0"))
	float DampingCoefficient = 0.5f;

	/**
	 * Linear drag coefficient in water (mass-independent acceleration).
	 * Scales with the fraction of sample points submerged.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Damping",
		meta = (ClampMin = "0.0", UIMin = "0.01", UIMax = "1.0"))
	float WaterDragLinear = 0.1f;

	/**
	 * Angular drag coefficient in water (mass-independent, radians).
	 * Scales with the fraction of sample points submerged.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Damping",
		meta = (ClampMin = "0.0", UIMin = "0.05", UIMax = "2.0"))
	float WaterDragAngular = 0.2f;

	// -------------------------------------------------------------------
	// Properties — Flags
	// -------------------------------------------------------------------

	/**
	 * Placeholder for future sailing / wind system integration.
	 * When true, indicates this body can be player-controlled on the water.
	 * No gameplay effect yet — reserved for wind force and rudder input.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy")
	bool bIsPossessable = false;

	/** True when any sample point is below the water surface. Read-only at runtime. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Buoyancy|State")
	bool bIsInWater = false;

	// -------------------------------------------------------------------
	// Sample Point Generator
	// -------------------------------------------------------------------

	/** Half-extents of the bounding box for sample point generation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Generator")
	FVector BoxHalfExtents = FVector(0.5f, 0.5f, 0.5f);

	/** Points per axis on the bottom face (2 = corners only, 3 = corners + edges + centre). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buoyancy|Generator",
		meta = (ClampMin = "2", ClampMax = "8", UIMin = "2", UIMax = "5"))
	int32 BoxPointsPerAxis = 3;

	/**
	 * Replace SamplePoints with an N×N grid on the bottom face of a box
	 * defined by BoxHalfExtents. Set BoxHalfExtents and BoxPointsPerAxis
	 * first, then click this button.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Buoyancy|Generator")
	void GenerateBoxSamplePoints();

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** Cached subsystem pointer — set once in BeginPlay. */
	UPROPERTY()
	TObjectPtr<UWaveParameterSubsystem> CachedSubsystem = nullptr;

	/** Cached root primitive for force application. */
	UPROPERTY()
	TObjectPtr<UPrimitiveComponent> PhysicsBody = nullptr;
};
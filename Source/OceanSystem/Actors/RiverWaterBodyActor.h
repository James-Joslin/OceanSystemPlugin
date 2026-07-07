// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RiverWaterBodyActor.generated.h"

class USplineComponent;
class USplineMeshComponent;
class UOceanBodyComponent;

/**
 * Water body actor for rivers defined by a spline path.
 *
 * The USplineComponent is the root and defines the river centreline.
 * Each segment between spline points becomes a USplineMeshComponent
 * deforming a flat grid plane along the curve. Gradient is handled
 * naturally via spline point Z positions.
 *
 * Unlike ocean/lake actors, rivers do NOT use UTiledWaterMeshComponent.
 * The spline mesh geometry follows the river path directly.
 */
UCLASS(meta = (DisplayName = "River Water Body"))
class OCEANSYSTEM_API ARiverWaterBodyActor : public AActor
{
	GENERATED_BODY()

public:
	ARiverWaterBodyActor();

	// -------------------------------------------------------------------
	// Components
	// -------------------------------------------------------------------

	/** Spline defining the river centreline. Root component. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "River")
	TObjectPtr<USplineComponent> RiverSpline;

	/** Water body component — registered with subsystem for wave eval. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "River")
	TObjectPtr<UOceanBodyComponent> OceanBody;

	// -------------------------------------------------------------------
	// River Properties
	// -------------------------------------------------------------------

	/** Cross-section width of the river in world units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River|Geometry",
		meta = (ClampMin = "10.0", UIMin = "50.0", UIMax = "5000.0"))
	float RiverWidth = 500.0f;

	/** Mesh subdivisions per spline segment (cross-section resolution). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River|Geometry",
		meta = (ClampMin = "2", ClampMax = "128", UIMin = "8", UIMax = "64"))
	int32 SegmentSubdivisions = 32;

	/** UV scroll speed along the spline tangent for flow direction.
		Pushed to the MID as "FlowSpeed" so the river material can
		animate texture coordinates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River|Flow",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "1000.0"))
	float FlowSpeed = 100.0f;

	// -------------------------------------------------------------------
	// Actions
	// -------------------------------------------------------------------

	/** Rebuild all spline mesh segments from the current spline. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "River")
	void RebuildRiverMesh();

	/** Reapply the MID to all spline mesh segments. */
	UFUNCTION(BlueprintCallable, Category = "River")
	void RefreshMeshMaterial();

protected:
	virtual void BeginPlay() override;

	/** Tick in editor so the viewport camera can interact with the river. */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Destroy all existing spline mesh segment components. */
	void DestroySegmentMeshes();

	/** Generate a flat grid mesh for a single spline segment.
		The mesh is a flat plane centred on the origin, RiverWidth wide
		and SegmentLength long, with SegmentSubdivisions resolution. */
	USplineMeshComponent* CreateSegmentMesh(
		int32 SegmentIndex,
		const FVector& StartPos, const FVector& StartTangent,
		const FVector& EndPos, const FVector& EndTangent);

	/** All spline mesh segments. Rebuilt on spline edit. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USplineMeshComponent>> SegmentMeshes;
};
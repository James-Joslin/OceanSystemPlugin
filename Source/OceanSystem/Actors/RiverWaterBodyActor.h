// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RiverWaterBodyActor.generated.h"

class USplineComponent;
class USplineMeshComponent;
class UOceanBodyComponent;
class UStaticMesh;

/**
 * Water body actor for rivers defined by a spline path.
 *
 * The USplineComponent is the root and defines the river centreline.
 * Each segment between spline points becomes a USplineMeshComponent
 * deforming a source mesh along the curve. Gradient is handled
 * naturally via spline point Z positions.
 *
 * The source mesh (RiverSegmentMesh) should be a flat subdivided
 * plane — author it in Blender at 100×100 units, subdivide to the
 * desired resolution, and export with LOD levels. The engine's
 * built-in static mesh LOD system handles distance-based transitions
 * automatically. SplineMeshComponent StartScale/EndScale drive the
 * actual river width from the RiverWidth property.
 *
 * Unlike ocean/lake actors, rivers do NOT use UTiledWaterMeshComponent.
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

	/** Subdivided plane mesh deformed along each spline segment.
		Author in Blender: 100×100 unit plane, subdivided, with LODs.
		The engine's static mesh LOD system handles distance transitions.
		If unset, falls back to the engine's BasicShapes/Plane (2 tris,
		no LODs — only useful for testing). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River|Geometry")
	TSoftObjectPtr<UStaticMesh> RiverSegmentMesh;

	/** Cross-section width of the river in world units.
		Drives SplineMeshComponent StartScale/EndScale relative to the
		source mesh's Y extent (100 units for the standard plane). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River|Geometry",
		meta = (ClampMin = "10.0", UIMin = "50.0", UIMax = "5000.0"))
	float RiverWidth = 500.0f;

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

	/** Update start/end positions, tangents, and width scale on existing
		segment meshes without creating or destroying components.
		Called from OnConstruction during spline point drags. */
	void UpdateSegmentTransforms();

	/** Create a spline mesh component for one spline segment. */
	USplineMeshComponent* CreateSegmentMesh(
		int32 SegmentIndex,
		const FVector& StartPos, const FVector& StartTangent,
		const FVector& EndPos, const FVector& EndTangent);

	/** Resolve the source mesh — user asset or engine fallback. */
	UStaticMesh* GetSegmentMesh() const;

	/** All spline mesh segments. Rebuilt on spline edit. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USplineMeshComponent>> SegmentMeshes;
};
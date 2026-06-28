// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "ClipmapOceanMeshComponent.generated.h"

/**
 * Camera-centred concentric LOD ring mesh for ocean rendering.
 *
 * Generates a clipmap: Ring 0 is a solid N×N grid at BaseCellSize.
 * Ring K (K > 0) is a hollow frame at BaseCellSize × 2^K with
 * stitching triangles on its inner edge to prevent T-junction cracks
 * against the finer ring inside it.
 *
 * The mesh is generated once (BeginPlay or CallInEditor). Per-frame,
 * the component snaps its XY to the camera position rounded to
 * SnapInterval. No mesh rebuild — geometry is static in local space,
 * only the transform moves.
 *
 * Total triangle count is constant regardless of ocean extent:
 *   Ring 0:  N² × 2
 *   Ring K:  (N² − (N/2)²) × 2  +  4 × (N/2) × 1  (stitching)
 *   With N=64, 6 rings ≈ 41k triangles.
 *
 * Attach alongside UOceanBodyComponent. The actor sets the MID on
 * all mesh sections via SetMaterial().
 */
UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UClipmapOceanMeshComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	UClipmapOceanMeshComponent(const FObjectInitializer& ObjectInitializer);

	// -------------------------------------------------------------------
	// Properties
	// -------------------------------------------------------------------

	/** Number of concentric rings. Each successive ring doubles cell size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clipmap",
		meta = (ClampMin = "1", ClampMax = "10", UIMin = "2", UIMax = "8"))
	int32 RingCount = 6;

	/**
	 * Cell size of the innermost ring (Ring 0) in world units.
	 * Ring K uses BaseCellSize × 2^K.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clipmap",
		meta = (ClampMin = "0.01", UIMin = "0.5", UIMax = "10.0"))
	float BaseCellSize = 1.0f;

	/**
	 * Grid cells per side for every ring. Must be divisible by 4
	 * (enforced on build) for correct stitching geometry.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clipmap",
		meta = (ClampMin = "4", ClampMax = "256", UIMin = "16", UIMax = "128"))
	int32 CellsPerRingSide = 64;

	/**
	 * Camera snap granularity in world units.
	 * Should match BaseCellSize to prevent sub-cell visual swimming.
	 * The component position snaps to multiples of this value.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clipmap",
		meta = (ClampMin = "0.001", UIMin = "0.5", UIMax = "10.0"))
	float SnapInterval = 1.0f;

	/**
	 * Extra vertical extent added to the mesh bounds in both directions.
	 * Must exceed the maximum wave amplitude to prevent frustum culling
	 * when the camera looks at the ocean from the side. Set this to at
	 * least MaxAmplitude × 1.5. The OceanWaterBodyActor can auto-set this
	 * from the wave config.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clipmap",
		meta = (ClampMin = "0.0", UIMin = "1.0", UIMax = "100.0"))
	float VerticalBoundsExtension = 50.0f;

	// -------------------------------------------------------------------
	// Build
	// -------------------------------------------------------------------

	/**
	 * Generate (or regenerate) the clipmap mesh.
	 * Clears all existing sections and rebuilds every ring.
	 * Called automatically on BeginPlay; available in editor via button.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Clipmap")
	void BuildClipmapMesh();

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	// -------------------------------------------------------------------
	// Mesh generation
	// -------------------------------------------------------------------

	/**
	 * Generate Ring 0 — a solid N×N grid of quads.
	 * Each quad is two CCW triangles with +Z normals.
	 *
	 * @param CellSize  World-unit size of each cell edge.
	 * @param N         Cells per side (= CellsPerRingSide).
	 */
	void GenerateSolidRing(
		float CellSize, int32 N,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs) const;

	/**
	 * Generate Ring K (K > 0) — a hollow frame with stitching.
	 *
	 * The frame is an N×N grid minus the inner (N/2)×(N/2) hole where
	 * the previous ring sits. Along each inner edge, stitching triangles
	 * add a midpoint vertex per cell to match the finer ring's vertex
	 * density and prevent T-junction cracks under WPO.
	 *
	 * @param RingIndex  Ring number (1+).
	 * @param CellSize   World-unit size of each cell edge at this ring level.
	 * @param N          Cells per side (= CellsPerRingSide).
	 */
	void GenerateFrameRing(
		int32 RingIndex, float CellSize, int32 N,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs) const;

	/** Whether the mesh has been built at least once. */
	bool bMeshBuilt = false;
};
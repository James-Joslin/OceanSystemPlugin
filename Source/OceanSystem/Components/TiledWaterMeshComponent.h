// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "TiledWaterMeshComponent.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

/**
 * Fixed-placement grid of flat tile meshes with camera-distance LOD.
 *
 * Used for lakes and wide river mouths. The component manages a grid of
 * TilesX × TilesY child UProceduralMeshComponents, each pre-generated
 * with mesh sections at every LOD level. Per-frame, each tile's distance
 * to the camera determines the active LOD section (all others are hidden).
 *
 * LOD levels:
 *   Level 0:  TileSubdivisions (highest detail, nearest)
 *   Level 1+: LODSubdivisions[0..N-1] (progressively coarser)
 *
 * The grid is centred on the component origin.
 *
 * Attach alongside UOceanBodyComponent. The actor sets the material on
 * all tile sections via SetMaterialOnAllTiles().
 */
UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UTiledWaterMeshComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UTiledWaterMeshComponent();

	// -------------------------------------------------------------------
	// Properties — Grid Layout
	// -------------------------------------------------------------------

	/** Number of tile columns (X axis). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TiledMesh|Grid",
		meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "8"))
	int32 TilesX = 4;

	/** Number of tile rows (Y axis). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TiledMesh|Grid",
		meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "8"))
	int32 TilesY = 4;

	/** World-unit size of each tile edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TiledMesh|Grid",
		meta = (ClampMin = "1.0", UIMin = "50.0", UIMax = "2000.0"))
	float TileSize = 500.0f;

	// -------------------------------------------------------------------
	// Properties — LOD
	// -------------------------------------------------------------------

	/** Subdivisions per tile edge at highest LOD (nearest to camera). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TiledMesh|LOD",
		meta = (ClampMin = "2", ClampMax = "128", UIMin = "16", UIMax = "128"))
	int32 TileSubdivisions = 64;

	/**
	 * Distance thresholds for LOD transitions.
	 * Tiles closer than LODDistances[0] use TileSubdivisions.
	 * Tiles between LODDistances[i-1] and LODDistances[i] use LODSubdivisions[i-1].
	 * Must have the same element count as LODSubdivisions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TiledMesh|LOD")
	TArray<float> LODDistances;

	/**
	 * Subdivision counts for each successive LOD level.
	 * Must have the same element count as LODDistances.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TiledMesh|LOD")
	TArray<int32> LODSubdivisions;

	/**
	 * Extra vertical extent added to each tile's bounds to prevent
	 * frustum culling under WPO. The actor should set this from the
	 * wave config's maximum displacement.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TiledMesh|Rendering",
		meta = (ClampMin = "0.0", UIMin = "1.0", UIMax = "100.0"))
	float VerticalBoundsExtension = 50.0f;

	// -------------------------------------------------------------------
	// Build
	// -------------------------------------------------------------------

	/**
	 * Generate (or regenerate) all tile meshes and LOD sections.
	 * Destroys existing tile components and creates new ones.
	 * Called automatically on BeginPlay; available in editor via button.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TiledMesh")
	void BuildTileMesh();

	// -------------------------------------------------------------------
	// Material
	// -------------------------------------------------------------------

	/**
	 * Apply a material to every LOD section of every tile.
	 * Called by the owning actor to wire the MID at BeginPlay.
	 */
	UFUNCTION(BlueprintCallable, Category = "TiledMesh")
	void SetMaterialOnAllTiles(UMaterialInterface* Material);

	/** Number of tile components currently created. */
	UFUNCTION(BlueprintCallable, Category = "TiledMesh")
	int32 GetTileCount() const { return TileMeshes.Num(); }

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	// -------------------------------------------------------------------
	// Mesh Generation
	// -------------------------------------------------------------------

	/**
	 * Generate a flat grid mesh centred on the origin.
	 *
	 * @param GridSize      World-unit extent of the grid in each axis.
	 * @param Subdivisions  Cells per side.
	 */
	void GenerateGridMesh(
		float GridSize, int32 Subdivisions,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs) const;

	/** Get the subdivision count for a LOD level index. */
	int32 GetSubdivisionsForLOD(int32 LODLevel) const;

	/** Total number of LOD levels (1 base + LODSubdivisions.Num()). */
	int32 GetNumLODLevels() const;

	/** Determine the LOD level for a given camera distance. */
	int32 ComputeLODLevel(float Distance) const;

	/** Destroy all tile mesh components and clear tracking arrays. */
	void DestroyTileMeshes();

	// -------------------------------------------------------------------
	// Tile Data
	// -------------------------------------------------------------------

	/** One ProceduralMeshComponent per tile, each with NumLODLevels sections. */
	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> TileMeshes;

	/** Current active LOD level per tile (parallel with TileMeshes). */
	TArray<int32> CurrentLODLevels;

	/** Whether tiles have been built at least once. */
	bool bMeshBuilt = false;
};
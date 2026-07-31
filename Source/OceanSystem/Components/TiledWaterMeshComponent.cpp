// Copyright James Joslin. All Rights Reserved.

#include "TiledWaterMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "CompGeom/Delaunay2.h"

#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#endif

namespace
{
	using FPoint2 = UE::Math::TVector2<double>;

	template <typename TInside, typename TIntersect>
	TArray<FPoint2> ClipPolygonHalfPlane(
		const TArray<FPoint2>& Input,
		TInside&& IsInside,
		TIntersect&& Intersect)
	{
		TArray<FPoint2> Output;
		if (Input.IsEmpty())
		{
			return Output;
		}

		FPoint2 Previous = Input.Last();
		bool bPreviousInside = IsInside(Previous);
		for (const FPoint2& Current : Input)
		{
			const bool bCurrentInside = IsInside(Current);
			if (bCurrentInside != bPreviousInside)
			{
				Output.Add(Intersect(Previous, Current));
			}
			if (bCurrentInside)
			{
				Output.Add(Current);
			}
			Previous = Current;
			bPreviousInside = bCurrentInside;
		}
		return Output;
	}

	TArray<FPoint2> ClipPolygonToSquare(
		const TArray<FPoint2>& Polygon,
		double HalfSize)
	{
		TArray<FPoint2> Result = Polygon;
		Result = ClipPolygonHalfPlane(Result,
			[HalfSize](const FPoint2& P) { return P.X >= -HalfSize; },
			[HalfSize](const FPoint2& A, const FPoint2& B)
			{
				const double T = (-HalfSize - A.X) / (B.X - A.X);
				return FPoint2(-HalfSize, FMath::Lerp(A.Y, B.Y, T));
			});
		Result = ClipPolygonHalfPlane(Result,
			[HalfSize](const FPoint2& P) { return P.X <= HalfSize; },
			[HalfSize](const FPoint2& A, const FPoint2& B)
			{
				const double T = (HalfSize - A.X) / (B.X - A.X);
				return FPoint2(HalfSize, FMath::Lerp(A.Y, B.Y, T));
			});
		Result = ClipPolygonHalfPlane(Result,
			[HalfSize](const FPoint2& P) { return P.Y >= -HalfSize; },
			[HalfSize](const FPoint2& A, const FPoint2& B)
			{
				const double T = (-HalfSize - A.Y) / (B.Y - A.Y);
				return FPoint2(FMath::Lerp(A.X, B.X, T), -HalfSize);
			});
		Result = ClipPolygonHalfPlane(Result,
			[HalfSize](const FPoint2& P) { return P.Y <= HalfSize; },
			[HalfSize](const FPoint2& A, const FPoint2& B)
			{
				const double T = (HalfSize - A.Y) / (B.Y - A.Y);
				return FPoint2(FMath::Lerp(A.X, B.X, T), HalfSize);
			});

		// Remove adjacent duplicates introduced when a cutout touches a tile edge.
		for (int32 Index = Result.Num() - 1; Index >= 0 && Result.Num() >= 2; --Index)
		{
			const int32 Previous = (Index - 1 + Result.Num()) % Result.Num();
			if (Result[Index].Equals(Result[Previous], 0.01))
			{
				Result.RemoveAt(Index);
			}
		}
		return Result;
	}
}

// ===================================================================
// Constructor
// ===================================================================

UTiledWaterMeshComponent::UTiledWaterMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// Default LOD chain: 5 levels (64 → 32 → 16 → 8 → 4)
	LODDistances = { 1000.0f, 2500.0f, 5000.0f, 8000.0f };
	LODSubdivisions = { 32, 16, 8, 4 };
}

// ===================================================================
// Lifecycle
// ===================================================================

void UTiledWaterMeshComponent::BeginPlay()
{
	Super::BeginPlay();

	// Always rebuild — tile meshes are transient and won't survive save/load.
	BuildTileMesh();
}

void UTiledWaterMeshComponent::TickComponent(
	float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (TileMeshes.IsEmpty())
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Determine LOD centre: in-game uses the possessed pawn (so camera
	// orbit doesn't inflate LOD), in-editor uses the active perspective
	// viewport camera so LODs update as you fly around the level.
	FVector LODCenter = FVector::ZeroVector;
	bool bHasCenter = false;

	if (World->IsGameWorld())
	{
		// Runtime / PIE: pawn location, fallback to camera
		const APlayerController* PC = World->GetFirstPlayerController();
		if (PC)
		{
			if (const APawn* Pawn = PC->GetPawn())
			{
				LODCenter = Pawn->GetActorLocation();
				bHasCenter = true;
			}
			else
			{
				FRotator CamRot;
				PC->GetPlayerViewPoint(LODCenter, CamRot);
				bHasCenter = true;
			}
		}
	}
#if WITH_EDITOR
	else
	{
		// Editor (non-PIE): use the active perspective viewport camera
		if (GEditor)
		{
			for (FEditorViewportClient* VC : GEditor->GetAllViewportClients())
			{
				if (VC && VC->IsPerspective())
				{
					LODCenter = VC->GetViewLocation();
					bHasCenter = true;
					break;
				}
			}
		}
	}
#endif

	if (!bHasCenter)
	{
		return;
	}

	const int32 NumLODs = GetNumLODLevels();

	for (int32 TileIdx = 0; TileIdx < TileMeshes.Num(); ++TileIdx)
	{
		UProceduralMeshComponent* Tile = TileMeshes[TileIdx];
		if (!Tile)
		{
			continue;
		}

		// Distance from pawn to tile centre (world space)
		const float Dist = FVector::Dist(LODCenter, Tile->GetComponentLocation());
		const int32 DesiredLOD = ComputeLODLevel(Dist);

		if (DesiredLOD != CurrentLODLevels[TileIdx])
		{
			// Hide old LOD section, show new one
			const int32 OldLOD = CurrentLODLevels[TileIdx];
			if (OldLOD >= 0 && OldLOD < NumLODs)
			{
				Tile->SetMeshSectionVisible(OldLOD, false);
			}
			if (DesiredLOD >= 0 && DesiredLOD < NumLODs)
			{
				Tile->SetMeshSectionVisible(DesiredLOD, true);
			}

			CurrentLODLevels[TileIdx] = DesiredLOD;
		}
	}
}

#if WITH_EDITOR
void UTiledWaterMeshComponent::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropName = PropertyChangedEvent.GetPropertyName();
	if (PropName == GET_MEMBER_NAME_CHECKED(UTiledWaterMeshComponent, TilesX)
		|| PropName == GET_MEMBER_NAME_CHECKED(UTiledWaterMeshComponent, TilesY)
		|| PropName == GET_MEMBER_NAME_CHECKED(UTiledWaterMeshComponent, TileSize)
		|| PropName == GET_MEMBER_NAME_CHECKED(UTiledWaterMeshComponent, TileSubdivisions)
		|| PropName == GET_MEMBER_NAME_CHECKED(UTiledWaterMeshComponent, LODDistances)
		|| PropName == GET_MEMBER_NAME_CHECKED(UTiledWaterMeshComponent, LODSubdivisions))
	{
		BuildTileMesh();
	}
	else if (PropName == GET_MEMBER_NAME_CHECKED(
		UTiledWaterMeshComponent, VerticalBoundsExtension))
	{
		RefreshBoundsScale();
	}
}
#endif

// ===================================================================
// BuildTileMesh
// ===================================================================

void UTiledWaterMeshComponent::BuildTileMesh()
{
	DestroyTileMeshes();

	// Validate LOD arrays — must be the same length
	if (LODDistances.Num() != LODSubdivisions.Num())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("TiledWaterMesh: LODDistances (%d) and LODSubdivisions (%d) "
				"must have the same element count. Truncating to shorter."),
			LODDistances.Num(), LODSubdivisions.Num());

		const int32 MinCount = FMath::Min(LODDistances.Num(), LODSubdivisions.Num());
		LODDistances.SetNum(MinCount);
		LODSubdivisions.SetNum(MinCount);
	}

	const int32 NumLODs = GetNumLODLevels();
	const int32 TotalTiles = TilesX * TilesY;

	// Grid is centred on the component origin
	const float GridHalfX = TilesX * TileSize * 0.5f;
	const float GridHalfY = TilesY * TileSize * 0.5f;

	TileMeshes.Reserve(TotalTiles);
	CurrentLODLevels.Init(0, TotalTiles);

	int32 TotalTriangles = 0;

	AActor* Owner = GetOwner();

	for (int32 iy = 0; iy < TilesY; ++iy)
	{
		for (int32 ix = 0; ix < TilesX; ++ix)
		{
			// Tile centre in parent-relative space
			const float CentreX = (ix + 0.5f) * TileSize - GridHalfX;
			const float CentreY = (iy + 0.5f) * TileSize - GridHalfY;

			// Create tile mesh component (transient — never serialised)
			UProceduralMeshComponent* Tile = NewObject<UProceduralMeshComponent>(
				Owner, NAME_None, RF_Transient);
			Tile->SetupAttachment(this);
			Tile->SetRelativeLocation(FVector(CentreX, CentreY, 0.0f));
			Tile->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Tile->SetCastShadow(false);
			Tile->bNeverDistanceCull = true;

			// Expand bounds vertically for WPO frustum culling.
			// BoundsScale applies uniformly so we compute a factor from
			// the vertical extension relative to the tile's horizontal extent.
			if (TileSize > UE_KINDA_SMALL_NUMBER)
			{
				const float Scale = 1.0f + (VerticalBoundsExtension * 2.0f) / TileSize;
				Tile->BoundsScale = FMath::Max(Scale, 1.0f);
			}

			Tile->RegisterComponent();

			// Generate mesh sections — one per LOD level
			const TArray<FColor> EmptyColors;

			for (int32 LOD = 0; LOD < NumLODs; ++LOD)
			{
				const int32 Subdivs = GetSubdivisionsForLOD(LOD);

				TArray<FVector> Vertices;
				TArray<int32> Triangles;
				TArray<FVector> Normals;
				TArray<FVector2D> UVs;
				TArray<FProcMeshTangent> Tangents;

				GenerateGridMesh(
					TileSize, Subdivs, FVector(CentreX, CentreY, 0.0f),
					Vertices, Triangles, Normals, UVs, Tangents);

				Tile->CreateMeshSection(LOD, Vertices, Triangles, Normals,
					UVs, EmptyColors, Tangents, /*bCreateCollision=*/false);

				// Only LOD 0 is visible initially
				Tile->SetMeshSectionVisible(LOD, LOD == 0);

				TotalTriangles += Triangles.Num() / 3;
			}

			TileMeshes.Add(Tile);
		}
	}

	bMeshBuilt = true;

	// Reapply cached material to new tiles — without this, rebuilt tiles
	// have no material and WPO/normals stop working until the actor
	// manually reapplies via SetMaterialOnAllTiles.
	if (CachedMaterial)
	{
		SetMaterialOnAllTiles(CachedMaterial);
	}

	UE_LOG(LogTemp, Log,
		TEXT("TiledWaterMesh: Built %d tiles (%dx%d), %d LOD levels, "
			"%d total triangles (all LODs), coverage %.1f × %.1f units."),
		TotalTiles, TilesX, TilesY, NumLODs, TotalTriangles,
		TilesX * TileSize, TilesY * TileSize);
}

// ===================================================================
// SetMaterialOnAllTiles
// ===================================================================

void UTiledWaterMeshComponent::SetMaterialOnAllTiles(UMaterialInterface* Material)
{
	if (!Material)
	{
		return;
	}

	// Cache so we can reapply after tile rebuilds
	CachedMaterial = Material;

	const int32 NumLODs = GetNumLODLevels();

	for (UProceduralMeshComponent* Tile : TileMeshes)
	{
		if (!Tile)
		{
			continue;
		}

		for (int32 LOD = 0; LOD < NumLODs; ++LOD)
		{
			Tile->SetMaterial(LOD, Material);
		}
	}
}

bool UTiledWaterMeshComponent::RegisterCutout(
	const FGuid& CutoutId,
	const TArray<FVector>& WorldBoundary)
{
	if (!CutoutId.IsValid() || WorldBoundary.Num() < 3)
	{
		return false;
	}

	TArray<FVector2D> Boundary;
	Boundary.Reserve(WorldBoundary.Num());
	for (const FVector& Point : WorldBoundary)
	{
		Boundary.Emplace(Point.X, Point.Y);
	}

	RegisteredCutouts.Add(CutoutId, MoveTemp(Boundary));
	BuildTileMesh();
	return true;
}

void UTiledWaterMeshComponent::UnregisterCutout(const FGuid& CutoutId)
{
	if (RegisteredCutouts.Remove(CutoutId) > 0)
	{
		BuildTileMesh();
	}
}

void UTiledWaterMeshComponent::RefreshBoundsScale()
{
	const float Scale = TileSize > UE_KINDA_SMALL_NUMBER
		? FMath::Max(1.0f + (VerticalBoundsExtension * 2.0f) / TileSize, 1.0f)
		: 1.0f;
	for (UProceduralMeshComponent* Tile : TileMeshes)
	{
		if (Tile)
		{
			Tile->BoundsScale = Scale;
			Tile->UpdateBounds();
			Tile->MarkRenderTransformDirty();
		}
	}
}

// ===================================================================
// LOD Helpers
// ===================================================================

int32 UTiledWaterMeshComponent::GetSubdivisionsForLOD(int32 LODLevel) const
{
	if (LODLevel <= 0)
	{
		return TileSubdivisions;
	}

	const int32 Idx = LODLevel - 1;
	if (Idx < LODSubdivisions.Num())
	{
		return FMath::Max(2, LODSubdivisions[Idx]);
	}

	// Beyond configured LOD levels — return the coarsest
	return LODSubdivisions.IsEmpty() ? TileSubdivisions : FMath::Max(2, LODSubdivisions.Last());
}

int32 UTiledWaterMeshComponent::GetNumLODLevels() const
{
	// Base level (TileSubdivisions) + one per LOD threshold
	return 1 + LODSubdivisions.Num();
}

int32 UTiledWaterMeshComponent::ComputeLODLevel(float Distance) const
{
	for (int32 i = 0; i < LODDistances.Num(); ++i)
	{
		if (Distance < LODDistances[i])
		{
			return i;
		}
	}

	// Beyond all thresholds — coarsest level
	return LODDistances.Num();
}

// ===================================================================
// Cleanup
// ===================================================================

void UTiledWaterMeshComponent::DestroyTileMeshes()
{
	for (TObjectPtr<UProceduralMeshComponent>& Tile : TileMeshes)
	{
		if (Tile)
		{
			Tile->DestroyComponent();
			Tile = nullptr;
		}
	}

	TileMeshes.Empty();
	CurrentLODLevels.Empty();
}

// ===================================================================
// Grid Mesh Generation
// ===================================================================
//
// Flat grid of Subdivisions × Subdivisions quads centred on the origin,
// spanning ±GridSize/2 in X and Y. Same geometry pattern as the clipmap
// Ring 0, parameterised by total extent instead of cell count + cell size.
//
// Winding: CCW from above → +Z normals.
// UVs: world-space (local position) for correct tiling under WPO.
// ===================================================================

void UTiledWaterMeshComponent::GenerateGridMesh(
	float GridSize, int32 Subdivisions,
	const FVector& TileRelativeCenter,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUVs,
	TArray<FProcMeshTangent>& OutTangents) const
{
	const int32 N = FMath::Max(2, Subdivisions);
	const int32 VN = N + 1;
	const float CellSize = GridSize / static_cast<float>(N);
	const float HalfSize = GridSize * 0.5f;

	// ----- Vertices -----
	const int32 VertCount = VN * VN;
	OutVertices.Reserve(VertCount);
	OutNormals.Reserve(VertCount);
	OutUVs.Reserve(VertCount);
	OutTangents.Reserve(VertCount);

	// Flat Z-up grid: tangent is always +X, bitangent derived as Normal × Tangent = +Y
	const FProcMeshTangent FlatTangent(FVector(1.0f, 0.0f, 0.0f), false);

	for (int32 j = 0; j < VN; ++j)
	{
		for (int32 i = 0; i < VN; ++i)
		{
			const float X = i * CellSize - HalfSize;
			const float Y = j * CellSize - HalfSize;

			OutVertices.Add(FVector(X, Y, 0.0f));
			OutNormals.Add(FVector::UpVector);

			// Normalised UV [0,1] per tile for standard texture sampling.
			// For seamless cross-tile sampling (ocean normals, foam), use
			// WorldPosition in the material instead of TexCoord.
			OutUVs.Add(FVector2D(
				static_cast<float>(i) / static_cast<float>(N),
				static_cast<float>(j) / static_cast<float>(N)));

			OutTangents.Add(FlatTangent);
		}
	}

	TArray<TArray<FPoint2>> ClippedCutouts;
	for (const TPair<FGuid, TArray<FVector2D>>& Pair : RegisteredCutouts)
	{
		TArray<FPoint2> LocalPolygon;
		LocalPolygon.Reserve(Pair.Value.Num());
		for (const FVector2D& WorldPoint : Pair.Value)
		{
			const FVector ComponentPoint = GetComponentTransform().InverseTransformPosition(
				FVector(WorldPoint.X, WorldPoint.Y, GetComponentLocation().Z));
			LocalPolygon.Emplace(
				ComponentPoint.X - TileRelativeCenter.X,
				ComponentPoint.Y - TileRelativeCenter.Y);
		}

		TArray<FPoint2> Clipped = ClipPolygonToSquare(LocalPolygon, HalfSize);
		if (Clipped.Num() >= 3)
		{
			ClippedCutouts.Add(MoveTemp(Clipped));
		}
	}

	// Fast path for ordinary tiles. They retain the original deterministic
	// grid topology and do not pay for constrained triangulation.
	if (ClippedCutouts.IsEmpty())
	{
		OutTriangles.Reserve(N * N * 6);
		for (int32 j = 0; j < N; ++j)
		{
			for (int32 i = 0; i < N; ++i)
			{
				const int32 BL = j * VN + i;
				const int32 BR = j * VN + (i + 1);
				const int32 TL = (j + 1) * VN + i;
				const int32 TR = (j + 1) * VN + (i + 1);
				OutTriangles.Append({ BL, BR, TR, BL, TR, TL });
			}
		}
		return;
	}

	// Constrained Delaunay keeps all normal grid vertices for WPO resolution,
	// while inserting exact connection edges as holes in the target surface.
	TArray<FPoint2> Points;
	Points.Reserve(OutVertices.Num() + 32);
	for (const FVector& Vertex : OutVertices)
	{
		Points.Emplace(Vertex.X, Vertex.Y);
	}

	TArray<UE::Geometry::FIndex2i> ConstraintEdges;
	TArray<int32> OuterLoop;
	OuterLoop.Reserve(N * 4);
	for (int32 i = 0; i <= N; ++i) OuterLoop.Add(i);
	for (int32 j = 1; j <= N; ++j) OuterLoop.Add(j * VN + N);
	for (int32 i = N - 1; i >= 0; --i) OuterLoop.Add(N * VN + i);
	for (int32 j = N - 1; j >= 1; --j) OuterLoop.Add(j * VN);
	for (int32 Index = 0; Index < OuterLoop.Num(); ++Index)
	{
		ConstraintEdges.Emplace(
			OuterLoop[Index], OuterLoop[(Index + 1) % OuterLoop.Num()]);
	}

	for (const TArray<FPoint2>& Cutout : ClippedCutouts)
	{
		const int32 StartIndex = Points.Num();
		for (const FPoint2& Point : Cutout)
		{
			Points.Add(Point);
			OutVertices.Emplace(Point.X, Point.Y, 0.0);
			OutNormals.Add(FVector::UpVector);
			OutUVs.Emplace(
				(Point.X + HalfSize) / GridSize,
				(Point.Y + HalfSize) / GridSize);
			OutTangents.Add(FlatTangent);
		}
		for (int32 Index = 0; Index < Cutout.Num(); ++Index)
		{
			ConstraintEdges.Emplace(
				StartIndex + Index,
				StartIndex + ((Index + 1) % Cutout.Num()));
		}
	}

	UE::Geometry::FDelaunay2 Delaunay;
	Delaunay.bAutomaticallyFixEdgesToDuplicateVertices = true;
	if (!Delaunay.Triangulate(Points, ConstraintEdges))
	{
		UE_LOG(LogTemp, Error,
			TEXT("TiledWaterMesh: constrained cutout triangulation failed; "
				"leaving the affected tile empty to prevent overlapping SLW surfaces."));
		OutTriangles.Empty();
		return;
	}

	const TArray<UE::Geometry::FIndex3i> Filled = Delaunay.GetFilledTriangles(
		ConstraintEdges, UE::Geometry::FDelaunay2::EFillMode::OddWinding);
	OutTriangles.Reserve(Filled.Num() * 3);
	for (const UE::Geometry::FIndex3i& Triangle : Filled)
	{
		OutTriangles.Append({ Triangle.A, Triangle.B, Triangle.C });
	}
}

// Copyright James Joslin. All Rights Reserved.

#include "ClipmapOceanMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

UClipmapOceanMeshComponent::UClipmapOceanMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	SetMobility(EComponentMobility::Movable);

	// No collision — buoyancy uses CPU wave evaluation, not physics traces
	bUseAsyncCooking = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Ocean should never be distance-culled
	bNeverDistanceCull = true;

	// Water doesn't cast shadows
	SetCastShadow(false);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void UClipmapOceanMeshComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!bMeshBuilt)
	{
		BuildClipmapMesh();
	}
}

void UClipmapOceanMeshComponent::TickComponent(
	float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}

	FVector CamLoc;
	FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);

	// Snap camera XY to grid — prevents sub-cell visual swimming.
	// GridSnap rounds to nearest multiple of SnapInterval.
	const float SnappedX = FMath::GridSnap(CamLoc.X, SnapInterval);
	const float SnappedY = FMath::GridSnap(CamLoc.Y, SnapInterval);

	// Preserve Z from the owning actor (water surface resting height).
	// The actor's Z is the BaseZ for wave evaluation.
	const FVector CurrentLoc = GetComponentLocation();
	const FVector NewLoc(SnappedX, SnappedY, CurrentLoc.Z);

	if (!NewLoc.Equals(CurrentLoc, UE_SMALL_NUMBER))
	{
		SetWorldLocation(NewLoc);
	}
}

#if WITH_EDITOR
void UClipmapOceanMeshComponent::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropName = PropertyChangedEvent.GetPropertyName();
	if (PropName == GET_MEMBER_NAME_CHECKED(UClipmapOceanMeshComponent, RingCount)
		|| PropName == GET_MEMBER_NAME_CHECKED(UClipmapOceanMeshComponent, BaseCellSize)
		|| PropName == GET_MEMBER_NAME_CHECKED(UClipmapOceanMeshComponent, CellsPerRingSide))
	{
		BuildClipmapMesh();
	}
}
#endif

// ---------------------------------------------------------------------------
// Bounds — expand vertically for WPO frustum culling
// ---------------------------------------------------------------------------

FBoxSphereBounds UClipmapOceanMeshComponent::CalcBounds(
	const FTransform& LocalToWorld) const
{
	// Start with the procedural mesh's own bounds (flat at Z=0).
	FBoxSphereBounds Base = Super::CalcBounds(LocalToWorld);

	// Expand vertically so the displaced surface isn't frustum-culled
	// when the camera looks from the side. The extension must exceed
	// the maximum wave amplitude.
	const FVector Expansion(0.0f, 0.0f, VerticalBoundsExtension);
	Base.BoxExtent += Expansion;
	Base.SphereRadius = Base.BoxExtent.Size();

	return Base;
}

// ===================================================================
// BuildClipmapMesh
// ===================================================================

void UClipmapOceanMeshComponent::BuildClipmapMesh()
{
	ClearAllMeshSections();

	// CellsPerRingSide must be divisible by 4 for stitching geometry.
	// Snap down if needed.
	if (CellsPerRingSide % 4 != 0)
	{
		CellsPerRingSide = FMath::Max(4, (CellsPerRingSide / 4) * 4);
		UE_LOG(LogTemp, Warning,
			TEXT("ClipmapOceanMesh: CellsPerRingSide snapped to %d (must be divisible by 4)."),
			CellsPerRingSide);
	}

	const int32 N = CellsPerRingSide;
	int32 TotalTriangles = 0;

	for (int32 Ring = 0; Ring < RingCount; ++Ring)
	{
		const float CellSize = BaseCellSize * static_cast<float>(1 << Ring);

		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;

		if (Ring == 0)
		{
			GenerateSolidRing(CellSize, N, Vertices, Triangles, Normals, UVs);
		}
		else
		{
			GenerateFrameRing(Ring, CellSize, N, Vertices, Triangles, Normals, UVs);
		}

		// Empty arrays for optional vertex data
		const TArray<FColor> EmptyColors;
		const TArray<FProcMeshTangent> EmptyTangents;

		CreateMeshSection(Ring, Vertices, Triangles, Normals, UVs,
			EmptyColors, EmptyTangents, /*bCreateCollision=*/false);

		TotalTriangles += Triangles.Num() / 3;
	}

	bMeshBuilt = true;

	// Compute coverage for log
	const float OuterHalf = (N / 2.0f) * BaseCellSize * static_cast<float>(1 << (RingCount - 1));
	const float Diameter = OuterHalf * 2.0f;

	UE_LOG(LogTemp, Log,
		TEXT("ClipmapOceanMesh: Built %d rings, %d triangles, diameter %.1f units (%.1fm)."),
		RingCount, TotalTriangles, Diameter, Diameter);
}

// ===================================================================
// Ring 0 — Solid Grid
// ===================================================================
//
// A flat N×N grid of quads at CellSize spacing, centred on the origin.
// Each quad is two triangles with CCW winding for +Z normals.
//
// Vertex layout (N+1 × N+1):
//   Position: ((i − N/2) × CellSize,  (j − N/2) × CellSize,  0)
//   UV:       (Position.X, Position.Y) — world-space tiling
//
// Triangle winding (CCW from above for +Z normal):
//   Tri 1: BL → BR → TR
//   Tri 2: BL → TR → TL
// ===================================================================

void UClipmapOceanMeshComponent::GenerateSolidRing(
	float CellSize, int32 N,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUVs) const
{
	const int32 VN = N + 1; // Vertices per side
	const float HalfN = N * 0.5f;

	// ----- Vertices -----
	const int32 VertexCount = VN * VN;
	OutVertices.Reserve(VertexCount);
	OutNormals.Reserve(VertexCount);
	OutUVs.Reserve(VertexCount);

	for (int32 j = 0; j < VN; ++j)
	{
		for (int32 i = 0; i < VN; ++i)
		{
			const float X = (i - HalfN) * CellSize;
			const float Y = (j - HalfN) * CellSize;

			OutVertices.Add(FVector(X, Y, 0.0f));
			OutNormals.Add(FVector::UpVector);
			OutUVs.Add(FVector2D(X, Y));
		}
	}

	// ----- Triangles -----
	const int32 TriIndexCount = N * N * 6; // 2 tris × 3 indices per cell
	OutTriangles.Reserve(TriIndexCount);

	for (int32 j = 0; j < N; ++j)
	{
		for (int32 i = 0; i < N; ++i)
		{
			const int32 BL = j * VN + i;
			const int32 BR = j * VN + (i + 1);
			const int32 TL = (j + 1) * VN + i;
			const int32 TR = (j + 1) * VN + (i + 1);

			// CCW winding → +Z normal
			OutTriangles.Add(BL);
			OutTriangles.Add(BR);
			OutTriangles.Add(TR);

			OutTriangles.Add(BL);
			OutTriangles.Add(TR);
			OutTriangles.Add(TL);
		}
	}
}

// ===================================================================
// Ring K > 0 — Hollow Frame with Stitching
// ===================================================================
//
// Geometry:
//   Full N×N grid minus an inner (N/2)×(N/2) hole where Ring K−1 sits.
//   The inner hole spans vertex indices [N/4, 3N/4] in both axes.
//
// Stitching:
//   At each inner hole edge, Ring K's cell size is 2× Ring K−1's.
//   A coarse cell edge spans two fine-ring cells, creating a T-junction.
//   Under WPO, the fine ring evaluates a vertex at the midpoint of the
//   coarse edge — if the coarse mesh interpolates linearly across that
//   span, the values differ and a crack appears.
//
//   Fix: for every frame cell adjacent to the hole, add a midpoint
//   vertex on the edge that touches the hole and tessellate with
//   3 triangles instead of 2. This matches the fine ring's vertex
//   density at the boundary.
//
//   Midpoint vertices are appended after the standard (N+1)² grid
//   vertices, indexed as:
//     Bottom edge midpoints: GridCount + 0..MidPerEdge-1
//     Top edge midpoints:    GridCount + MidPerEdge..2*MidPerEdge-1
//     Left edge midpoints:   GridCount + 2*MidPerEdge..3*MidPerEdge-1
//     Right edge midpoints:  GridCount + 3*MidPerEdge..4*MidPerEdge-1
//
//   where MidPerEdge = InnerMax − InnerMin = N/2.
//
// Cell classification:
//   Hole:    i ∈ [InnerMin, InnerMax−1] AND j ∈ [InnerMin, InnerMax−1]
//            → skip (no triangles)
//
//   Bottom stitch: j == InnerMin−1, i ∈ [InnerMin, InnerMax−1]
//            → 3 tris with top-edge midpoint
//
//   Top stitch:    j == InnerMax,   i ∈ [InnerMin, InnerMax−1]
//            → 3 tris with bottom-edge midpoint
//
//   Left stitch:   i == InnerMin−1, j ∈ [InnerMin, InnerMax−1]
//            → 3 tris with right-edge midpoint
//
//   Right stitch:  i == InnerMax,   j ∈ [InnerMin, InnerMax−1]
//            → 3 tris with left-edge midpoint
//
//   Frame (all other):
//            → 2 tris, standard quad
//
// No cell belongs to two stitch edges — the corner cells of the hole
// (e.g. (InnerMin−1, InnerMin−1)) fall outside all stitch ranges.
// ===================================================================

void UClipmapOceanMeshComponent::GenerateFrameRing(
	int32 RingIndex, float CellSize, int32 N,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUVs) const
{
	const int32 VN = N + 1;
	const float HalfN = N * 0.5f;

	// Inner hole boundaries (vertex indices)
	const int32 InnerMin = N / 4;       // First vertex on hole edge
	const int32 InnerMax = 3 * N / 4;   // Last vertex on hole edge (inclusive)

	// Number of midpoint vertices per hole edge
	const int32 MidPerEdge = InnerMax - InnerMin; // = N/2

	// ---------------------------------------------------------------
	// 1. Vertices
	// ---------------------------------------------------------------

	const int32 GridCount = VN * VN;
	const int32 TotalVerts = GridCount + 4 * MidPerEdge;

	OutVertices.Reserve(TotalVerts);
	OutNormals.Reserve(TotalVerts);
	OutUVs.Reserve(TotalVerts);

	// --- Standard grid vertices (same layout as Ring 0) ---
	for (int32 j = 0; j < VN; ++j)
	{
		for (int32 i = 0; i < VN; ++i)
		{
			const float X = (i - HalfN) * CellSize;
			const float Y = (j - HalfN) * CellSize;

			OutVertices.Add(FVector(X, Y, 0.0f));
			OutNormals.Add(FVector::UpVector);
			OutUVs.Add(FVector2D(X, Y));
		}
	}

	// --- Stitching midpoint vertices ---
	// Each midpoint sits at the centre of a coarse cell edge that
	// borders the hole, matching the fine ring's vertex position.

	// Bottom hole edge (j_vertex = InnerMin): midpoints on cells below hole
	// These are the top-edge midpoints of cells at j_cell = InnerMin−1
	const int32 BottomMidStart = GridCount;
	for (int32 i = InnerMin; i < InnerMax; ++i)
	{
		const float X = (i + 0.5f - HalfN) * CellSize;
		const float Y = (InnerMin - HalfN) * CellSize;
		OutVertices.Add(FVector(X, Y, 0.0f));
		OutNormals.Add(FVector::UpVector);
		OutUVs.Add(FVector2D(X, Y));
	}

	// Top hole edge (j_vertex = InnerMax): midpoints on cells above hole
	// These are the bottom-edge midpoints of cells at j_cell = InnerMax
	const int32 TopMidStart = BottomMidStart + MidPerEdge;
	for (int32 i = InnerMin; i < InnerMax; ++i)
	{
		const float X = (i + 0.5f - HalfN) * CellSize;
		const float Y = (InnerMax - HalfN) * CellSize;
		OutVertices.Add(FVector(X, Y, 0.0f));
		OutNormals.Add(FVector::UpVector);
		OutUVs.Add(FVector2D(X, Y));
	}

	// Left hole edge (i_vertex = InnerMin): midpoints on cells left of hole
	// These are the right-edge midpoints of cells at i_cell = InnerMin−1
	const int32 LeftMidStart = TopMidStart + MidPerEdge;
	for (int32 j = InnerMin; j < InnerMax; ++j)
	{
		const float X = (InnerMin - HalfN) * CellSize;
		const float Y = (j + 0.5f - HalfN) * CellSize;
		OutVertices.Add(FVector(X, Y, 0.0f));
		OutNormals.Add(FVector::UpVector);
		OutUVs.Add(FVector2D(X, Y));
	}

	// Right hole edge (i_vertex = InnerMax): midpoints on cells right of hole
	// These are the left-edge midpoints of cells at i_cell = InnerMax
	const int32 RightMidStart = LeftMidStart + MidPerEdge;
	for (int32 j = InnerMin; j < InnerMax; ++j)
	{
		const float X = (InnerMax - HalfN) * CellSize;
		const float Y = (j + 0.5f - HalfN) * CellSize;
		OutVertices.Add(FVector(X, Y, 0.0f));
		OutNormals.Add(FVector::UpVector);
		OutUVs.Add(FVector2D(X, Y));
	}

	check(OutVertices.Num() == TotalVerts);

	// ---------------------------------------------------------------
	// 2. Triangles
	// ---------------------------------------------------------------

	// Estimate: frame cells (N²−(N/2)²) × 6 indices + stitch cells × 9
	const int32 FrameCells = N * N - MidPerEdge * MidPerEdge;
	const int32 StitchCells = 4 * MidPerEdge;
	const int32 EstIndices = (FrameCells - StitchCells) * 6 + StitchCells * 9;
	OutTriangles.Reserve(EstIndices);

	// Helper: grid vertex index from (i, j) grid coordinates
	auto GridIdx = [VN](int32 i, int32 j) -> int32
		{
			return j * VN + i;
		};

	for (int32 jc = 0; jc < N; ++jc)
	{
		for (int32 ic = 0; ic < N; ++ic)
		{
			// --- Skip cells inside the hole ---
			if (ic >= InnerMin && ic < InnerMax &&
				jc >= InnerMin && jc < InnerMax)
			{
				continue;
			}

			// Corner vertices of this cell
			const int32 BL = GridIdx(ic, jc);
			const int32 BR = GridIdx(ic + 1, jc);
			const int32 TL = GridIdx(ic, jc + 1);
			const int32 TR = GridIdx(ic + 1, jc + 1);

			// --- Check stitching edges ---

			// Bottom stitch: cell's top edge borders hole's bottom edge
			const bool bBottomStitch =
				(jc == InnerMin - 1) &&
				(ic >= InnerMin && ic < InnerMax);

			// Top stitch: cell's bottom edge borders hole's top edge
			const bool bTopStitch =
				(jc == InnerMax) &&
				(ic >= InnerMin && ic < InnerMax);

			// Left stitch: cell's right edge borders hole's left edge
			const bool bLeftStitch =
				(ic == InnerMin - 1) &&
				(jc >= InnerMin && jc < InnerMax);

			// Right stitch: cell's left edge borders hole's right edge
			const bool bRightStitch =
				(ic == InnerMax) &&
				(jc >= InnerMin && jc < InnerMax);

			if (bBottomStitch)
			{
				// Midpoint on top edge (hole's bottom edge)
				const int32 TM = BottomMidStart + (ic - InnerMin);

				// 3 triangles (all CCW → +Z)
				//   TL --- TM --- TR
				//    \    / \    /
				//     \  /   \  /
				//      \/     \/
				//   BL --------- BR
				OutTriangles.Add(BL);
				OutTriangles.Add(TM);
				OutTriangles.Add(TL);

				OutTriangles.Add(BL);
				OutTriangles.Add(BR);
				OutTriangles.Add(TM);

				OutTriangles.Add(BR);
				OutTriangles.Add(TR);
				OutTriangles.Add(TM);
			}
			else if (bTopStitch)
			{
				// Midpoint on bottom edge (hole's top edge)
				const int32 BM = TopMidStart + (ic - InnerMin);

				// 3 triangles (all CCW → +Z)
				//   TL --------- TR
				//      /\     /\
				//     /  \   /  \
				//    /    \ /    \
				//   BL --- BM --- BR
				OutTriangles.Add(TL);
				OutTriangles.Add(BL);
				OutTriangles.Add(BM);

				OutTriangles.Add(TL);
				OutTriangles.Add(BM);
				OutTriangles.Add(TR);

				OutTriangles.Add(BM);
				OutTriangles.Add(BR);
				OutTriangles.Add(TR);
			}
			else if (bLeftStitch)
			{
				// Midpoint on right edge (hole's left edge)
				const int32 RM = LeftMidStart + (jc - InnerMin);

				// 3 triangles (all CCW → +Z)
				//   TL --- TR
				//    |    /|
				//    |  /  |
				//    |/ RM |
				//    |  \  |
				//    |    \|
				//   BL --- BR
				OutTriangles.Add(BL);
				OutTriangles.Add(BR);
				OutTriangles.Add(RM);

				OutTriangles.Add(BL);
				OutTriangles.Add(RM);
				OutTriangles.Add(TL);

				OutTriangles.Add(TL);
				OutTriangles.Add(RM);
				OutTriangles.Add(TR);
			}
			else if (bRightStitch)
			{
				// Midpoint on left edge (hole's right edge)
				const int32 LM = RightMidStart + (jc - InnerMin);

				// 3 triangles (all CCW → +Z)
				//   TL --- TR
				//    |\    |
				//    |  \  |
				//    | LM \|
				//    |  /  |
				//    |/    |
				//   BL --- BR
				OutTriangles.Add(LM);
				OutTriangles.Add(BL);
				OutTriangles.Add(BR);

				OutTriangles.Add(LM);
				OutTriangles.Add(BR);
				OutTriangles.Add(TR);

				OutTriangles.Add(LM);
				OutTriangles.Add(TR);
				OutTriangles.Add(TL);
			}
			else
			{
				// --- Regular frame cell: 2 triangles ---
				OutTriangles.Add(BL);
				OutTriangles.Add(BR);
				OutTriangles.Add(TR);

				OutTriangles.Add(BL);
				OutTriangles.Add(TR);
				OutTriangles.Add(TL);
			}
		}
	}
}
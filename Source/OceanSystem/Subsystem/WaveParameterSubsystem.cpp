// Copyright James Joslin. All Rights Reserved.

#include "WaveParameterSubsystem.h"
#include "../Components/OceanBodyComponent.h"
#include "../Components/WaterBodyJunctionComponent.h"
#include "Components/SplineComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Texture2D.h"
#include "Misc/App.h"

// STATGROUP_OceanSystem is declared in Types/OceanTypes.h (shared across the plugin).
DECLARE_CYCLE_STAT(TEXT("WaveSubsystem Tick"), STAT_WaveSubsystemTick, STATGROUP_OceanSystem);
DECLARE_CYCLE_STAT(TEXT("WaveSubsystem Eval Physics"), STAT_WaveSubsystemEvalPhysics, STATGROUP_OceanSystem);
DECLARE_CYCLE_STAT(TEXT("WaveSubsystem Eval Full"), STAT_WaveSubsystemEvalFull, STATGROUP_OceanSystem);
DECLARE_CYCLE_STAT(TEXT("WaveSubsystem Eval Velocity"), STAT_WaveSubsystemEvalVelocity, STATGROUP_OceanSystem);

// ---------------------------------------------------------------------------
// MID parameter name cache
// ---------------------------------------------------------------------------

namespace
{
	/** Half-step for the central finite difference in GetWaterVelocity.
		At 1/240 s the amplitude error is under 0.2% even on the fastest
		10 cm detail ripples (omega ~ 25 rad/s); the swells that actually
		drive spray are exact to float precision. */
	constexpr float GWaveVelocityHalfStep = 1.0f / 240.0f;
}

// ---------------------------------------------------------------------------
// D0 verification logging (Ocean.Debug.WaterVelocity console command)
//
// Cross-checks GetWaterVelocity against a per-frame finite difference of
// GetFullWaveHeight at a fixed world point. The Z components should track
// closely; with a body's TimeScale = 0 both should read ~zero.
// ---------------------------------------------------------------------------
namespace OceanVelDebug
{
	static bool    bActive = false;
	static double  Until = 0.0;
	static FVector Location = FVector::ZeroVector;
	static float   PrevHeight = 0.0f;
	static bool    bHavePrev = false;
}

static void TickWaterVelocityDebug(UWaveParameterSubsystem& Subsystem, const UWorld& World, float DeltaTime);

namespace OceanMID
{
	static const FName WaveCountName(TEXT("WaveCount"));
	static const FName WaveTimeName(TEXT("WaveTime"));
	static const FName WaveDataTexName(TEXT("WaveDataTexture"));

	// Detail layers — per-pixel normal evaluation
	static const FName DetailWaveCountName(TEXT("DetailWaveCount"));
	static const FName DetailWaveDataTexName(TEXT("DetailWaveDataTexture"));

	// Visual shaping — pushed from the component so the shader always
	// matches the CPU evaluator (single source of truth for parity)
	static const FName CrestSharpnessName(TEXT("CrestSharpness"));
	static const FName DomainWarpFrequencyName(TEXT("DomainWarpFrequency"));
	static const FName DomainWarpAmountName(TEXT("DomainWarpAmount"));

	// Blend zone — depth fade width for material-side edge blending
	static const FName BlendWidthName(TEXT("BlendWidth"));

	// River — flow speed for UV scrolling along spline tangent
	static const FName FlowSpeedName(TEXT("FlowSpeed"));
}

// ===================================================================
// Lifecycle
// ===================================================================

void UWaveParameterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	WaterBodies.Empty();
	WaterConnections.Empty();
}

void UWaveParameterSubsystem::Deinitialize()
{
	WaterConnections.Empty();
	WaterBodies.Empty();
	Super::Deinitialize();
}

TStatId UWaveParameterSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWaveParameterSubsystem, STATGROUP_OceanSystem);
}

// ===================================================================
// Tick — MID sync
// ===================================================================

void UWaveParameterSubsystem::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_WaveSubsystemTick);

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// In-game: world time (respects pause, dilation).
	// In editor (outside PIE): wall-clock time so wave preview animates.
	const float WorldTime = World->IsGameWorld()
		? World->GetTimeSeconds()
		: static_cast<float>(FApp::GetCurrentTime());

	for (FWaterBodyEntry& Entry : WaterBodies)
	{
		SyncMaterialInstance(Entry, WorldTime);
	}

	// Body textures are created/synchronised first. Junction MIDs can then
	// safely bind both source and target textures without owning duplicates.
	for (FWaterBodyConnectionEntry& Connection : WaterConnections)
	{
		SyncConnectionMaterial(Connection, WorldTime);
	}

	if (OceanVelDebug::bActive)
	{
		TickWaterVelocityDebug(*this, *World, DeltaTime);
	}
}

// ===================================================================
// Water Body Registry
// ===================================================================

void UWaveParameterSubsystem::RegisterWaterBody(const FWaterBodyEntry& Entry)
{
	// Avoid double-registration
	if (Entry.Owner.IsValid() && FindEntryIndex(Entry.Owner.Get()) != INDEX_NONE)
	{
		return;
	}

	FWaterBodyEntry NewEntry = Entry;

	// Enforce amplitude-descending layer sort
	NewEntry.WaveConfig.SortLayers();
	NewEntry.WaveConfig.bDirty = true;
	NewEntry.DetailWaveConfig.SortLayers();
	NewEntry.DetailWaveConfig.bDirty = true;

	const int32 NewIndex = WaterBodies.Add(MoveTemp(NewEntry));

	SortRegistryByPriority();

	// Re-find index after sort (position may have changed)
	const int32 SortedIndex = FindEntryIndex(Entry.Owner.Get());
	if (SortedIndex != INDEX_NONE)
	{
		DetectBlendZones(SortedIndex);
	}

	OnWaterBodiesChanged.Broadcast();
}

void UWaveParameterSubsystem::UnregisterWaterBody(const UOceanBodyComponent* Body)
{
	if (!Body)
	{
		return;
	}

	RemoveBlendZonesFor(Body);

	const int32 Index = FindEntryIndex(Body);
	if (Index != INDEX_NONE)
	{
		WaterBodies.RemoveAtSwap(Index);
		// RemoveAtSwap may invalidate ordering; re-sort
		SortRegistryByPriority();
	}

	OnWaterBodiesChanged.Broadcast();
}

void UWaveParameterSubsystem::UpdateWaterBodyConfig(
	const UOceanBodyComponent* Body, const FWaveConfig& NewConfig)
{
	const int32 Index = FindEntryIndex(Body);
	if (Index == INDEX_NONE)
	{
		return;
	}

	FWaterBodyEntry& Entry = WaterBodies[Index];
	Entry.WaveConfig = NewConfig;
	Entry.WaveConfig.SortLayers();
	Entry.WaveConfig.bDirty = true;
}

void UWaveParameterSubsystem::UpdateDetailWaveConfig(
	const UOceanBodyComponent* Body, const FWaveConfig& NewDetailConfig)
{
	const int32 Index = FindEntryIndex(Body);
	if (Index == INDEX_NONE)
	{
		return;
	}

	FWaterBodyEntry& Entry = WaterBodies[Index];
	Entry.DetailWaveConfig = NewDetailConfig;
	Entry.DetailWaveConfig.SortLayers();
	Entry.DetailWaveConfig.bDirty = true;
}

void UWaveParameterSubsystem::MarkBodyDirty(const UOceanBodyComponent* Body)
{
	const int32 Index = FindEntryIndex(Body);
	if (Index != INDEX_NONE)
	{
		WaterBodies[Index].WaveConfig.bDirty = true;
	}
}

void UWaveParameterSubsystem::RegisterWaterConnection(
	UOceanBodyComponent* SourceBody,
	const FWaterBodyConnectionConfig& Config,
	UMaterialInstanceDynamic* JunctionMID,
	UWaterBodyJunctionComponent* JunctionComponent)
{
	if (!IsValid(SourceBody) || !Config.IsUsable()
		|| !IsValid(Config.TargetBody)
		|| SourceBody == Config.TargetBody
		|| SourceBody->BodyType != EOceanBodyType::River
		|| Config.TargetBody->BodyType == EOceanBodyType::River)
	{
		return;
	}

	FGuid ConnectionId = Config.ConnectionId;
	if (!ConnectionId.IsValid())
	{
		ConnectionId = FGuid::NewGuid();
	}

	FWaterBodyConnectionEntry* Existing = WaterConnections.FindByPredicate(
		[&ConnectionId](const FWaterBodyConnectionEntry& Entry)
		{
			return Entry.ConnectionId == ConnectionId;
		});

	FWaterBodyConnectionEntry NewEntry;
	NewEntry.ConnectionId = ConnectionId;
	NewEntry.NetworkId = Config.NetworkId.IsValid() ? Config.NetworkId : ConnectionId;
	NewEntry.SourceBody = SourceBody;
	NewEntry.TargetBody = Config.TargetBody;
	NewEntry.Endpoint = Config.Endpoint;
	NewEntry.BlendLength = FMath::Max(Config.BlendLength, 10.0f);
	NewEntry.MouthWidthScale = FMath::Max(Config.MouthWidthScale, 0.25f);
	NewEntry.JunctionMID = JunctionMID;
	NewEntry.JunctionComponent = JunctionComponent;

	if (Existing)
	{
		*Existing = MoveTemp(NewEntry);
	}
	else
	{
		WaterConnections.Add(MoveTemp(NewEntry));
	}
}

void UWaveParameterSubsystem::UnregisterWaterConnection(const FGuid& ConnectionId)
{
	WaterConnections.RemoveAll([&ConnectionId](const FWaterBodyConnectionEntry& Entry)
	{
		return Entry.ConnectionId == ConnectionId;
	});
}

void UWaveParameterSubsystem::UpdateWaterConnectionGeometry(
	const FGuid& ConnectionId,
	const FVector& WorldStart,
	const FVector& WorldDirection,
	const FVector& WorldRight,
	float StartHalfWidth,
	float EndHalfWidth,
	float Length)
{
	FWaterBodyConnectionEntry* Entry = WaterConnections.FindByPredicate(
		[&ConnectionId](const FWaterBodyConnectionEntry& Candidate)
		{
			return Candidate.ConnectionId == ConnectionId;
		});
	if (!Entry)
	{
		return;
	}

	Entry->WorldStart = WorldStart;
	Entry->WorldDirection = WorldDirection.GetSafeNormal2D();
	Entry->WorldRight = WorldRight.GetSafeNormal2D();
	Entry->StartHalfWidth = FMath::Max(StartHalfWidth, 1.0f);
	Entry->EndHalfWidth = FMath::Max(EndHalfWidth, 1.0f);
	Entry->BlendLength = FMath::Max(Length, 10.0f);
	Entry->bHasJunctionGeometry = !Entry->WorldDirection.IsNearlyZero()
		&& !Entry->WorldRight.IsNearlyZero();
}

void UWaveParameterSubsystem::UnregisterConnectionsFor(const UOceanBodyComponent* Body)
{
	WaterConnections.RemoveAll([Body](const FWaterBodyConnectionEntry& Entry)
	{
		return Entry.SourceBody.Get() == Body || Entry.TargetBody.Get() == Body;
	});
}

void UWaveParameterSubsystem::RefreshConnectionsFor(
	const UOceanBodyComponent* Body)
{
	if (!Body)
	{
		return;
	}

	TArray<TWeakObjectPtr<UWaterBodyJunctionComponent>> Junctions;
	for (const FWaterBodyConnectionEntry& Connection : WaterConnections)
	{
		if ((Connection.SourceBody.Get() == Body
				|| Connection.TargetBody.Get() == Body)
			&& Connection.JunctionComponent.IsValid())
		{
			Junctions.AddUnique(Connection.JunctionComponent);
		}
	}

	// Work from a snapshot because each rebuild updates its registry entry.
	for (const TWeakObjectPtr<UWaterBodyJunctionComponent>& Junction : Junctions)
	{
		if (Junction.IsValid())
		{
			Junction->RebuildJunction();
		}
	}
}

const FWaterBodyEntry* UWaveParameterSubsystem::GetWaterBodyEntry(
	const UOceanBodyComponent* Body) const
{
	const int32 Index = FindEntryIndex(Body);
	return WaterBodies.IsValidIndex(Index) ? &WaterBodies[Index] : nullptr;
}

// ===================================================================
// Spatial Queries
// ===================================================================

const FWaterBodyEntry* UWaveParameterSubsystem::FindWaterBodyAt(const FVector2D& XY) const
{
	// Bodies are sorted by priority descending — first match is highest priority
	for (const FWaterBodyEntry& Entry : WaterBodies)
	{
		if (!Entry.Owner.IsValid())
		{
			continue;
		}
		if (IsPointInBody(Entry, XY))
		{
			return &Entry;
		}
	}
	return nullptr;
}

FWaterBodyQueryResult UWaveParameterSubsystem::QueryBodiesAt(const FVector& WorldPos) const
{
	FWaterBodyQueryResult Result;
	const FVector2D XY(WorldPos.X, WorldPos.Y);

	// Collect all bodies containing this point
	TArray<int32, TInlineAllocator<4>> ContainingIndices;

	for (int32 i = 0; i < WaterBodies.Num(); ++i)
	{
		const FWaterBodyEntry& Entry = WaterBodies[i];
		if (!Entry.Owner.IsValid())
		{
			continue;
		}
		if (IsPointInBody(Entry, XY))
		{
			ContainingIndices.Add(i);
		}
	}

	if (ContainingIndices.IsEmpty())
	{
		return Result;
	}

	// Primary = first (highest priority, since registry is sorted descending)
	Result.PrimaryIndex = ContainingIndices[0];

	// Check for blend zone with the next-lower-priority body
	if (ContainingIndices.Num() >= 2)
	{
		const int32 SecondaryIdx = ContainingIndices[1];
		const FWaterBodyEntry& Primary = WaterBodies[Result.PrimaryIndex];
		const FWaterBodyEntry& Secondary = WaterBodies[SecondaryIdx];

		// Blend alpha based on distance from the primary body's edge.
		// Near the primary's edge (small distance) → blend toward secondary.
		// Deep inside primary (large distance) → pure primary.
		const float DistFromPrimaryEdge = DistanceFromBodyEdge(Primary, XY);

		// Find the blend width — check the primary's blend zones for a
		// matching pair, fall back to a default.
		float BlendWidth = 200.0f;
		for (const FBlendZoneEntry& BZ : Primary.BlendZones)
		{
			const UOceanBodyComponent* BZOther =
				(BZ.BodyA == Primary.Owner) ? BZ.BodyB.Get() : BZ.BodyA.Get();
			if (BZOther == Secondary.Owner.Get())
			{
				BlendWidth = BZ.BlendWidth;
				break;
			}
		}

		if (DistFromPrimaryEdge < BlendWidth)
		{
			Result.SecondaryIndex = SecondaryIdx;
			// smoothstep-like blend: 0 at edge → 1 at BlendWidth inside
			const float T = FMath::Clamp(DistFromPrimaryEdge / BlendWidth, 0.0f, 1.0f);
			// Invert: near edge (T≈0) → more secondary; deep inside (T≈1) → pure primary
			Result.BlendAlpha = 1.0f - FMath::SmoothStep(0.0f, 1.0f, T);
		}
	}

	return Result;
}

// ===================================================================
// CPU Evaluation — Physics Path
// ===================================================================

bool UWaveParameterSubsystem::GetWaveHeight(const FVector& WorldPos, float& OutWorldZ)
{
	FGerstnerResult Result;
	if (!GetWaveData(WorldPos, Result))
	{
		return false;
	}
	OutWorldZ = Result.WorldZ;
	return true;
}

bool UWaveParameterSubsystem::GetWaveData(const FVector& WorldPos, FGerstnerResult& OutResult)
{
	SCOPE_CYCLE_COUNTER(STAT_WaveSubsystemEvalPhysics);

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	return EvaluateSurfaceAt(
		WorldPos, World->GetTimeSeconds(), /*bFullEval=*/false, OutResult);
}

// ===================================================================
// CPU Evaluation — Full Path
// ===================================================================

bool UWaveParameterSubsystem::GetFullWaveHeight(const FVector& WorldPos, float& OutWorldZ)
{
	SCOPE_CYCLE_COUNTER(STAT_WaveSubsystemEvalFull);

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FGerstnerResult Result;
	if (!EvaluateSurfaceAt(
		WorldPos, World->GetTimeSeconds(), /*bFullEval=*/true, Result))
	{
		return false;
	}

	OutWorldZ = Result.WorldZ;
	return true;
}

bool UWaveParameterSubsystem::GetWaterVelocity(const FVector& WorldPos, FVector& OutVelocity)
{
	SCOPE_CYCLE_COUNTER(STAT_WaveSubsystemEvalVelocity);

	OutVelocity = FVector::ZeroVector;

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const float WorldTime = World->GetTimeSeconds();
	FGerstnerResult Before;
	FGerstnerResult After;
	if (!EvaluateSurfaceAt(
		WorldPos, WorldTime - GWaveVelocityHalfStep, /*bFullEval=*/true, Before)
		|| !EvaluateSurfaceAt(
			WorldPos, WorldTime + GWaveVelocityHalfStep, /*bFullEval=*/true, After))
	{
		return false;
	}

	OutVelocity =
		(After.Displacement - Before.Displacement)
		/ (2.0f * GWaveVelocityHalfStep);

	return true;
}

float UWaveParameterSubsystem::GetFoldIntensity(const FVector& WorldPos)
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0f;
	}

	FGerstnerResult Result;
	return EvaluateSurfaceAt(
		WorldPos, World->GetTimeSeconds(), /*bFullEval=*/true, Result)
		? Result.FoldIntensity
		: 0.0f;
}

// ===================================================================
// Internal — Helpers
// ===================================================================

int32 UWaveParameterSubsystem::FindEntryIndex(const UOceanBodyComponent* Body) const
{
	for (int32 i = 0; i < WaterBodies.Num(); ++i)
	{
		if (WaterBodies[i].Owner.Get() == Body)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void UWaveParameterSubsystem::SortRegistryByPriority()
{
	WaterBodies.Sort([](const FWaterBodyEntry& A, const FWaterBodyEntry& B)
		{
			return A.Priority > B.Priority;
		});
}

bool UWaveParameterSubsystem::IsPointInBody(const FWaterBodyEntry& Entry, const FVector2D& XY) const
{
	switch (Entry.BodyType)
	{
	case EOceanBodyType::Ocean:
		// Ocean is infinite — XY bounds check is a soft hint but ocean
		// is the base layer and always considered to contain any point.
		// If the bounds are valid and huge, check them; otherwise accept.
		if (Entry.Bounds.bIsValid)
		{
			return Entry.Bounds.IsInside(XY);
		}
		return true;

	case EOceanBodyType::Lake:
		return Entry.Bounds.bIsValid && Entry.Bounds.IsInside(XY);

	case EOceanBodyType::River:
	{
		const USplineComponent* Spline = Entry.SplineData.Get();
		if (!Spline)
		{
			return false;
		}
		const FVector ClosestPoint = Spline->FindLocationClosestToWorldLocation(
			FVector(XY.X, XY.Y, 0.0f), ESplineCoordinateSpace::World);
		const float DistSq = FVector2D::DistSquared(
			XY, FVector2D(ClosestPoint.X, ClosestPoint.Y));
		return DistSq <= FMath::Square(Entry.RiverHalfWidth);
	}

	default:
		return false;
	}
}

float UWaveParameterSubsystem::DistanceFromBodyEdge(
	const FWaterBodyEntry& Entry, const FVector2D& XY) const
{
	switch (Entry.BodyType)
	{
	case EOceanBodyType::Ocean:
		// Ocean has no edge — return large value so blending never triggers
		// from the ocean side. Other bodies blend over ocean, not the reverse.
		return TNumericLimits<float>::Max();

	case EOceanBodyType::Lake:
	{
		if (!Entry.Bounds.bIsValid)
		{
			return 0.0f;
		}
		// Distance from XY to the nearest edge of the AABB.
		// Positive = inside, negative = outside.
		const FVector2D& Min = Entry.Bounds.Min;
		const FVector2D& Max = Entry.Bounds.Max;
		const float DX = FMath::Min(XY.X - Min.X, Max.X - XY.X);
		const float DY = FMath::Min(XY.Y - Min.Y, Max.Y - XY.Y);
		return FMath::Min(DX, DY);
	}

	case EOceanBodyType::River:
	{
		const USplineComponent* Spline = Entry.SplineData.Get();
		if (!Spline)
		{
			return 0.0f;
		}
		const FVector ClosestPoint = Spline->FindLocationClosestToWorldLocation(
			FVector(XY.X, XY.Y, 0.0f), ESplineCoordinateSpace::World);
		const float Dist2D = FVector2D::Distance(
			XY, FVector2D(ClosestPoint.X, ClosestPoint.Y));
		// Positive = inside river, negative = outside
		return Entry.RiverHalfWidth - Dist2D;
	}

	default:
		return 0.0f;
	}
}

// ===================================================================
// Internal — Blend Zone Detection
// ===================================================================

void UWaveParameterSubsystem::DetectBlendZones(int32 NewBodyIndex)
{
	if (!WaterBodies.IsValidIndex(NewBodyIndex))
	{
		return;
	}

	FWaterBodyEntry& NewEntry = WaterBodies[NewBodyIndex];

	for (int32 i = 0; i < WaterBodies.Num(); ++i)
	{
		if (i == NewBodyIndex)
		{
			continue;
		}

		FWaterBodyEntry& Other = WaterBodies[i];

		// Check spatial overlap
		bool bOverlaps = false;

		if (NewEntry.BodyType == EOceanBodyType::River || Other.BodyType == EOceanBodyType::River)
		{
			// River overlap: simplified check — does the spline's bounding
			// box (expanded by half-width) intersect the other body's bounds?
			const FWaterBodyEntry& RiverEntry =
				(NewEntry.BodyType == EOceanBodyType::River) ? NewEntry : Other;
			const FWaterBodyEntry& FlatEntry =
				(NewEntry.BodyType == EOceanBodyType::River) ? Other : NewEntry;

			const USplineComponent* Spline = RiverEntry.SplineData.Get();
			if (Spline && FlatEntry.Bounds.bIsValid)
			{
				const FBox SplineBounds = Spline->Bounds.GetBox();
				const FBox2D SplineBounds2D(
					FVector2D(SplineBounds.Min.X - RiverEntry.RiverHalfWidth,
						SplineBounds.Min.Y - RiverEntry.RiverHalfWidth),
					FVector2D(SplineBounds.Max.X + RiverEntry.RiverHalfWidth,
						SplineBounds.Max.Y + RiverEntry.RiverHalfWidth));
				bOverlaps = SplineBounds2D.Intersect(FlatEntry.Bounds);
			}
			else if (FlatEntry.BodyType == EOceanBodyType::Ocean)
			{
				// Ocean is infinite — always overlaps with rivers
				bOverlaps = true;
			}
		}
		else
		{
			// Both flat (ocean/lake) — AABB overlap check
			if (NewEntry.Bounds.bIsValid && Other.Bounds.bIsValid)
			{
				bOverlaps = NewEntry.Bounds.Intersect(Other.Bounds);
			}
			else if (NewEntry.BodyType == EOceanBodyType::Ocean
				|| Other.BodyType == EOceanBodyType::Ocean)
			{
				// Ocean is infinite — overlaps with any other body
				bOverlaps = true;
			}
		}

		if (bOverlaps)
		{
			// Use the smaller of the two bodies' blend widths so the
			// narrower body controls the transition zone.
			const float WidthA = NewEntry.Owner.IsValid()
				? NewEntry.Owner->BlendWidth : 200.0f;
			const float WidthB = Other.Owner.IsValid()
				? Other.Owner->BlendWidth : 200.0f;

			FBlendZoneEntry BZ;
			BZ.BodyA = NewEntry.Owner;
			BZ.BodyB = Other.Owner;
			BZ.BlendWidth = FMath::Min(WidthA, WidthB);
			BZ.BlendType = EBlendType::DepthFade;

			NewEntry.BlendZones.Add(BZ);
			Other.BlendZones.Add(BZ);
		}
	}

	// ---- River endpoint auto-detection ----
	// If the newly registered body is a river, check whether each spline
	// endpoint falls inside any other water body. If so, create a blend
	// zone automatically so the river mouth transitions smoothly into the
	// ocean or lake without requiring manual overlap placement.
	if (NewEntry.BodyType == EOceanBodyType::River && NewEntry.SplineData.IsValid())
	{
		const USplineComponent* Spline = NewEntry.SplineData.Get();
		const float SplineLength = Spline->GetSplineLength();

		// Test both endpoints (start and end of the spline)
		for (int32 EndpointIdx = 0; EndpointIdx < 2; ++EndpointIdx)
		{
			const float Distance = (EndpointIdx == 0) ? 0.0f : SplineLength;
			const FVector EndpointWorld = Spline->GetLocationAtDistanceAlongSpline(
				Distance, ESplineCoordinateSpace::World);
			const FVector2D EndpointXY(EndpointWorld.X, EndpointWorld.Y);

			for (int32 i = 0; i < WaterBodies.Num(); ++i)
			{
				if (i == NewBodyIndex)
				{
					continue;
				}

				const FWaterBodyEntry& Other = WaterBodies[i];
				if (!Other.Owner.IsValid())
				{
					continue;
				}

				// Skip if a blend zone between these two already exists
				bool bAlreadyBlended = false;
				for (const FBlendZoneEntry& ExistingBZ : NewEntry.BlendZones)
				{
					if ((ExistingBZ.BodyA == NewEntry.Owner && ExistingBZ.BodyB == Other.Owner) ||
						(ExistingBZ.BodyA == Other.Owner && ExistingBZ.BodyB == NewEntry.Owner))
					{
						bAlreadyBlended = true;
						break;
					}
				}
				if (bAlreadyBlended)
				{
					continue;
				}

				// If the river endpoint is inside this other body, create a
				// blend zone — the river flows into or out of this body.
				if (IsPointInBody(Other, EndpointXY))
				{
					const float WidthRiver = NewEntry.Owner.IsValid()
						? NewEntry.Owner->BlendWidth : 200.0f;
					const float WidthOther = Other.Owner.IsValid()
						? Other.Owner->BlendWidth : 200.0f;

					FBlendZoneEntry BZ;
					BZ.BodyA = NewEntry.Owner;
					BZ.BodyB = Other.Owner;
					BZ.BlendWidth = FMath::Min(WidthRiver, WidthOther);
					BZ.BlendType = EBlendType::DepthFade;

					WaterBodies[NewBodyIndex].BlendZones.Add(BZ);
					WaterBodies[i].BlendZones.Add(BZ);
				}
			}
		}
	}
}

void UWaveParameterSubsystem::RemoveBlendZonesFor(const UOceanBodyComponent* Body)
{
	for (FWaterBodyEntry& Entry : WaterBodies)
	{
		Entry.BlendZones.RemoveAll([Body](const FBlendZoneEntry& BZ)
			{
				return BZ.BodyA.Get() == Body || BZ.BodyB.Get() == Body;
			});
	}
}

// ===================================================================
// Internal — Evaluation Dispatch
// ===================================================================

float UWaveParameterSubsystem::ComputeConnectionAlpha(
	const FWaterBodyConnectionEntry& Connection,
	const FVector& WorldPos) const
{
	if (Connection.bHasJunctionGeometry)
	{
		const FVector Delta = WorldPos - Connection.WorldStart;
		const float Along = FVector::DotProduct(Delta, Connection.WorldDirection);
		const float T = FMath::Clamp(
			Along / FMath::Max(Connection.BlendLength, 10.0f), 0.0f, 1.0f);
		return WaterConnectionMath::SmootherStep01(T);
	}

	const UOceanBodyComponent* SourceBody = Connection.SourceBody.Get();
	const FWaterBodyEntry* SourceEntry = GetWaterBodyEntry(SourceBody);
	const USplineComponent* Spline = SourceEntry
		? SourceEntry->SplineData.Get()
		: nullptr;
	if (!Spline || Spline->IsClosedLoop())
	{
		return 0.0f;
	}

	const float InputKey = Spline->FindInputKeyClosestToWorldLocation(WorldPos);
	const float Distance = Spline->GetDistanceAlongSplineAtSplineInputKey(InputKey);
	const float SplineLength = Spline->GetSplineLength();
	const float BlendLength = FMath::Clamp(
		Connection.BlendLength, 10.0f, FMath::Max(SplineLength, 10.0f));

	float T = Connection.Endpoint == EWaterConnectionEndpoint::End
		? (Distance - (SplineLength - BlendLength)) / BlendLength
		: Distance / BlendLength;
	T = FMath::Clamp(T, 0.0f, 1.0f);

	// Quintic smootherstep: value and first derivative are stable at both
	// snapped geometry boundaries. Mirrored in WaterBodyConnection.ush.
	const float Smooth = WaterConnectionMath::SmootherStep01(T);
	return Connection.Endpoint == EWaterConnectionEndpoint::End
		? Smooth
		: 1.0f - Smooth;
}

const FWaterBodyConnectionEntry* UWaveParameterSubsystem::FindConnectionAt(
	const FVector& WorldPos,
	int32& OutSourceIndex,
	int32& OutTargetIndex,
	float& OutAlpha) const
{
	OutSourceIndex = INDEX_NONE;
	OutTargetIndex = INDEX_NONE;
	OutAlpha = 0.0f;

	const FVector2D XY(WorldPos.X, WorldPos.Y);
	const FWaterBodyConnectionEntry* Best = nullptr;
	int32 BestPriority = TNumericLimits<int32>::Lowest();

	for (const FWaterBodyConnectionEntry& Connection : WaterConnections)
	{
		const int32 SourceIndex = FindEntryIndex(Connection.SourceBody.Get());
		const int32 TargetIndex = FindEntryIndex(Connection.TargetBody.Get());
		if (!WaterBodies.IsValidIndex(SourceIndex)
			|| !WaterBodies.IsValidIndex(TargetIndex))
		{
			continue;
		}

		const FWaterBodyEntry& Source = WaterBodies[SourceIndex];
		const FWaterBodyEntry& Target = WaterBodies[TargetIndex];
		if (Source.BodyType != EOceanBodyType::River
			|| Target.BodyType == EOceanBodyType::River)
		{
			continue;
		}

		bool bInsideConnection = false;
		if (Connection.bHasJunctionGeometry)
		{
			const FVector Delta = WorldPos - Connection.WorldStart;
			const float Along = FVector::DotProduct(Delta, Connection.WorldDirection);
			const float Length = FMath::Max(Connection.BlendLength, 10.0f);
			if (Along >= 0.0f && Along <= Length)
			{
				const float LinearT = Along / Length;
				const float HalfWidth = FMath::Lerp(
					Connection.StartHalfWidth,
					Connection.EndHalfWidth,
					LinearT);
				const float Across = FMath::Abs(
					FVector::DotProduct(Delta, Connection.WorldRight));
				bInsideConnection = Across <= HalfWidth;
			}
		}
		else
		{
			bInsideConnection = IsPointInBody(Source, XY)
				&& IsPointInBody(Target, XY);
		}

		if (!bInsideConnection)
		{
			continue;
		}

		const float Alpha = ComputeConnectionAlpha(Connection, WorldPos);
		if (Alpha <= UE_KINDA_SMALL_NUMBER)
		{
			continue;
		}

		if (!Best || Source.Priority > BestPriority)
		{
			Best = &Connection;
			BestPriority = Source.Priority;
			OutSourceIndex = SourceIndex;
			OutTargetIndex = TargetIndex;
			OutAlpha = Alpha;
		}
	}

	return Best;
}

bool UWaveParameterSubsystem::EvaluateSurfaceAt(
	const FVector& WorldPos,
	float WorldTime,
	bool bFullEval,
	FGerstnerResult& OutResult) const
{
	int32 SourceIndex = INDEX_NONE;
	int32 TargetIndex = INDEX_NONE;
	float ConnectionAlpha = 0.0f;
	if (FindConnectionAt(
		WorldPos, SourceIndex, TargetIndex, ConnectionAlpha))
	{
		const FGerstnerResult Source = EvaluateBody(
			WaterBodies[SourceIndex], WorldPos, WorldTime, bFullEval);
		const FGerstnerResult Target = EvaluateBody(
			WaterBodies[TargetIndex], WorldPos, WorldTime, bFullEval);
		const float Alpha = FMath::Clamp(ConnectionAlpha, 0.0f, 1.0f);

		OutResult.Displacement = FMath::Lerp(
			Source.Displacement, Target.Displacement, Alpha);
		OutResult.Normal = FMath::Lerp(
			Source.Normal, Target.Normal, Alpha).GetSafeNormal();
		OutResult.WorldZ = WaterConnectionMath::BlendAbsoluteHeight(
			Source.WorldZ, Target.WorldZ, Alpha);
		OutResult.FoldIntensity = FMath::Lerp(
			Source.FoldIntensity, Target.FoldIntensity, Alpha);
		return true;
	}

	const FWaterBodyQueryResult Query = QueryBodiesAt(WorldPos);
	if (!Query.IsValid())
	{
		return false;
	}

	const FGerstnerResult Primary = EvaluateBody(
		WaterBodies[Query.PrimaryIndex], WorldPos, WorldTime, bFullEval);
	if (!Query.IsBlending())
	{
		OutResult = Primary;
		return true;
	}

	const FGerstnerResult Secondary = EvaluateBody(
		WaterBodies[Query.SecondaryIndex], WorldPos, WorldTime, bFullEval);
	const float Alpha = FMath::Clamp(Query.BlendAlpha, 0.0f, 1.0f);
	OutResult.Displacement = FMath::Lerp(
		Primary.Displacement, Secondary.Displacement, Alpha);
	OutResult.Normal = FMath::Lerp(
		Primary.Normal, Secondary.Normal, Alpha).GetSafeNormal();
	OutResult.WorldZ = WaterConnectionMath::BlendAbsoluteHeight(
		Primary.WorldZ, Secondary.WorldZ, Alpha);
	OutResult.FoldIntensity = FMath::Lerp(
		Primary.FoldIntensity, Secondary.FoldIntensity, Alpha);
	return true;
}

FGerstnerResult UWaveParameterSubsystem::EvaluateBody(
	const FWaterBodyEntry& Entry, const FVector& WorldPos,
	float WorldTime, bool bFullEval) const
{
	switch (Entry.BodyType)
	{
	case EOceanBodyType::Ocean:
	case EOceanBodyType::Lake:
		return bFullEval
			? FGerstnerEvaluator::EvaluateVisual(WorldPos, WorldTime, Entry.BaseZ,
				Entry.WaveConfig, Entry.DomainWarpFrequency,
				Entry.DomainWarpAmount, Entry.CrestSharpness)
			: FGerstnerEvaluator::EvaluatePhysicsVisual(WorldPos, WorldTime, Entry.BaseZ,
				Entry.WaveConfig, Entry.DomainWarpFrequency,
				Entry.DomainWarpAmount, Entry.CrestSharpness);

	case EOceanBodyType::River:
	{
		const USplineComponent* Spline = Entry.SplineData.Get();
		if (Spline)
		{
			// River: visual spline evaluators handle domain warp, crest
			// sharpening, and spline BaseZ lookup internally — full GPU
			// parity on the CPU path.
			return bFullEval
				? FGerstnerEvaluator::EvaluateVisualAlongSpline(
					WorldPos, WorldTime, Spline, Entry.WaveConfig,
					Entry.DomainWarpFrequency, Entry.DomainWarpAmount,
					Entry.CrestSharpness)
				: FGerstnerEvaluator::EvaluatePhysicsVisualAlongSpline(
					WorldPos, WorldTime, Spline, Entry.WaveConfig,
					Entry.DomainWarpFrequency, Entry.DomainWarpAmount,
					Entry.CrestSharpness);
		}
		// Fallback: flat visual eval at BaseZ
		return bFullEval
			? FGerstnerEvaluator::EvaluateVisual(WorldPos, WorldTime, Entry.BaseZ,
				Entry.WaveConfig, Entry.DomainWarpFrequency,
				Entry.DomainWarpAmount, Entry.CrestSharpness)
			: FGerstnerEvaluator::EvaluatePhysicsVisual(WorldPos, WorldTime, Entry.BaseZ,
				Entry.WaveConfig, Entry.DomainWarpFrequency,
				Entry.DomainWarpAmount, Entry.CrestSharpness);
	}

	default:
		return FGerstnerResult();
	}
}

// ===================================================================
// Internal — MID Sync (data texture path)
// ===================================================================
//
// Parameter packing:
//   Scalar  "WaveCount"            — number of active main layers
//   Scalar  "WaveTime"             — world time × TimeScale
//   Texture "WaveDataTexture"      — 16×2 RGBA32F main wave data
//   Scalar  "DetailWaveCount"      — number of active detail layers
//   Texture "DetailWaveDataTexture" — 16×2 RGBA32F detail wave data
//
// Main and detail textures are created lazily and updated independently
// only when their respective config is dirty. Each frame only the time
// scalar is pushed — no texture upload unless dirty.
//
// Texture layout (same for both):
//   Row 0, Pixel N = (Amplitude, Wavelength, Steepness, Speed)
//   Row 1, Pixel N = (Direction.X, Direction.Y, PhaseOffset, 0)
//
// The .ush reads via Texture2D.Load(int3(N, row, 0)).
// ===================================================================

void UWaveParameterSubsystem::SyncMaterialInstance(FWaterBodyEntry& Entry, float WorldTime)
{
	UMaterialInstanceDynamic* MID = Entry.MaterialInstance.Get();
	if (!MID)
	{
		return;
	}

	// Always push time (scaled by this body's TimeScale)
	MID->SetScalarParameterValue(
		OceanMID::WaveTimeName,
		WorldTime * Entry.WaveConfig.TimeScale);

	const bool bMainDirty = Entry.WaveConfig.bDirty;
	const bool bDetailDirty = Entry.DetailWaveConfig.bDirty;

	// Nothing to sync if neither config changed
	if (!bMainDirty && !bDetailDirty)
	{
		return;
	}

	// ---- Main wave data texture ----
	if (bMainDirty)
	{
		if (!Entry.WaveDataTexture.IsValid())
		{
			Entry.WaveDataTexture = CreateWaveDataTexture();
		}

		UTexture2D* Tex = Entry.WaveDataTexture.Get();
		if (Tex)
		{
			UpdateWaveDataTexture(Tex, Entry.WaveConfig);

			const int32 LayerCount = Entry.WaveConfig.Layers.Num();
			MID->SetScalarParameterValue(OceanMID::WaveCountName, static_cast<float>(LayerCount));
			MID->SetTextureParameterValue(OceanMID::WaveDataTexName, Tex);
		}

		// Push visual shaping so the shader can never drift from the CPU
		// evaluator. The material must expose these as scalar parameters
		// and feed them into the custom node — do NOT hardcode values in
		// the material graph. (Editing CrestSharpness on the component
		// previously only changed the CPU side; the rendered surface kept
		// its own value, producing a permanent height offset.)
		MID->SetScalarParameterValue(OceanMID::CrestSharpnessName, Entry.CrestSharpness);
		MID->SetScalarParameterValue(OceanMID::DomainWarpFrequencyName, Entry.DomainWarpFrequency);
		MID->SetScalarParameterValue(OceanMID::DomainWarpAmountName, Entry.DomainWarpAmount);

		// Push blend width for material-side depth fade at body edges.
		// The material reads this scalar and fades opacity where depth
		// below the surface is less than BlendWidth.
		const float BlendWidth = Entry.Owner.IsValid()
			? Entry.Owner->BlendWidth : 200.0f;
		MID->SetScalarParameterValue(OceanMID::BlendWidthName, BlendWidth);

		// Push flow speed for river UV scrolling. Zero for ocean/lake —
		// the material ignores it when unused.
		MID->SetScalarParameterValue(OceanMID::FlowSpeedName, Entry.FlowSpeed);

		Entry.WaveConfig.bDirty = false;
	}

	// ---- Detail wave data texture ----
	if (bDetailDirty)
	{
		if (Entry.DetailWaveConfig.Layers.Num() > 0)
		{
			if (!Entry.DetailWaveDataTexture.IsValid())
			{
				Entry.DetailWaveDataTexture = CreateWaveDataTexture();
			}

			UTexture2D* DetailTex = Entry.DetailWaveDataTexture.Get();
			if (DetailTex)
			{
				UpdateWaveDataTexture(DetailTex, Entry.DetailWaveConfig);

				const int32 DetailCount = Entry.DetailWaveConfig.Layers.Num();
				MID->SetScalarParameterValue(OceanMID::DetailWaveCountName, static_cast<float>(DetailCount));
				MID->SetTextureParameterValue(OceanMID::DetailWaveDataTexName, DetailTex);
			}
		}
		else
		{
			// No detail layers — zero the count so the shader skips the loop
			MID->SetScalarParameterValue(OceanMID::DetailWaveCountName, 0.0f);
		}

		Entry.DetailWaveConfig.bDirty = false;
	}
}

void UWaveParameterSubsystem::SyncConnectionMaterial(
	FWaterBodyConnectionEntry& Connection,
	float WorldTime)
{
	UMaterialInstanceDynamic* MID = Connection.JunctionMID.Get();
	const int32 SourceIndex = FindEntryIndex(Connection.SourceBody.Get());
	const int32 TargetIndex = FindEntryIndex(Connection.TargetBody.Get());
	if (!MID || !WaterBodies.IsValidIndex(SourceIndex)
		|| !WaterBodies.IsValidIndex(TargetIndex))
	{
		if (MID)
		{
			MID->SetScalarParameterValue(TEXT("ConnectionEnabled"), 0.0f);
		}
		return;
	}

	const FWaterBodyEntry& Source = WaterBodies[SourceIndex];
	const FWaterBodyEntry& Target = WaterBodies[TargetIndex];

	auto PushBody = [MID, WorldTime](
		const TCHAR* Prefix,
		const FWaterBodyEntry& Entry)
	{
		const FString P(Prefix);
		auto Name = [&P](const TCHAR* Suffix)
		{
			return FName(*(P + Suffix));
		};

		MID->SetScalarParameterValue(
			Name(TEXT("WaveCount")),
			static_cast<float>(Entry.WaveConfig.GetTotalLayerCount()));
		MID->SetScalarParameterValue(
			Name(TEXT("WaveTime")),
			WorldTime * Entry.WaveConfig.TimeScale);
		MID->SetScalarParameterValue(Name(TEXT("BaseZ")), Entry.BaseZ);
		MID->SetScalarParameterValue(
			Name(TEXT("CrestSharpness")), Entry.CrestSharpness);
		MID->SetScalarParameterValue(
			Name(TEXT("DomainWarpFrequency")), Entry.DomainWarpFrequency);
		MID->SetScalarParameterValue(
			Name(TEXT("DomainWarpAmount")), Entry.DomainWarpAmount);

		if (UTexture2D* WaveTexture = Entry.WaveDataTexture.Get())
		{
			MID->SetTextureParameterValue(
				Name(TEXT("WaveDataTexture")), WaveTexture);
		}

		MID->SetScalarParameterValue(
			Name(TEXT("DetailWaveCount")),
			static_cast<float>(Entry.DetailWaveConfig.GetTotalLayerCount()));
		if (UTexture2D* DetailTexture = Entry.DetailWaveDataTexture.Get())
		{
			MID->SetTextureParameterValue(
				Name(TEXT("DetailWaveDataTexture")), DetailTexture);
		}
	};

	MID->SetScalarParameterValue(TEXT("ConnectionEnabled"), 1.0f);
	MID->SetScalarParameterValue(
		TEXT("ConnectionEndpoint"),
		Connection.Endpoint == EWaterConnectionEndpoint::End ? 1.0f : 0.0f);
	MID->SetScalarParameterValue(
		TEXT("ConnectionBlendLength"), Connection.BlendLength);

	PushBody(TEXT("Source"), Source);
	PushBody(TEXT("Target"), Target);
}

// ===================================================================
// Internal — Wave Data Texture
// ===================================================================

UTexture2D* UWaveParameterSubsystem::CreateWaveDataTexture() const
{
	constexpr int32 Width = FWaveConfig::MaxLayers;   // 16
	constexpr int32 Height = 2;

	UTexture2D* Tex = UTexture2D::CreateTransient(Width, Height, PF_A32B32G32R32F);
	if (!Tex)
	{
		UE_LOG(LogTemp, Error, TEXT("WaveParameterSubsystem: Failed to create wave data texture."));
		return nullptr;
	}

	Tex->Filter = TF_Nearest;
	Tex->SRGB = 0;
	Tex->CompressionSettings = TC_HDR;
	Tex->MipGenSettings = TMGS_NoMipmaps;
	Tex->AddressX = TA_Clamp;
	Tex->AddressY = TA_Clamp;
	Tex->NeverStream = true;

	// Allocate and zero the pixel buffer
	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	Mip.BulkData.Lock(LOCK_READ_WRITE);
	void* Pixels = Mip.BulkData.Realloc(Width * Height * sizeof(FLinearColor));
	FMemory::Memzero(Pixels, Width * Height * sizeof(FLinearColor));
	Mip.BulkData.Unlock();
	Tex->UpdateResource();

	return Tex;
}

void UWaveParameterSubsystem::UpdateWaveDataTexture(
	UTexture2D* Texture, const FWaveConfig& Config) const
{
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
	{
		return;
	}

	const int32 Width = Texture->GetSizeX();
	const int32 LayerCount = FMath::Min(Config.Layers.Num(), Width);

	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* RawData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FLinearColor* Pixels = static_cast<FLinearColor*>(RawData);

	// Row 0: (Amplitude, Wavelength, Steepness, Speed)
	// Row 1: (Direction.X, Direction.Y, PhaseOffset, 0)
	for (int32 i = 0; i < LayerCount; ++i)
	{
		const FGerstnerWaveLayer& L = Config.Layers[i];
		Pixels[i] = FLinearColor(L.Amplitude, L.Wavelength, L.Steepness, L.Speed);
		Pixels[Width + i] = FLinearColor(L.Direction.X, L.Direction.Y, L.PhaseOffset, 0.0f);
	}

	// Zero unused slots to prevent stale data
	for (int32 i = LayerCount; i < Width; ++i)
	{
		Pixels[i] = FLinearColor::Black;
		Pixels[Width + i] = FLinearColor::Black;
	}

	Mip.BulkData.Unlock();
	Texture->UpdateResource();
}
// ===================================================================
// Debug: Ocean.Debug.WaterVelocity
// ===================================================================

static void TickWaterVelocityDebug(UWaveParameterSubsystem& Subsystem, const UWorld& World, float DeltaTime)
{
	using namespace OceanVelDebug;

	if (World.GetTimeSeconds() > Until)
	{
		bActive = false;
		bHavePrev = false;
		UE_LOG(LogTemp, Log, TEXT("OceanVelDebug: done."));
		return;
	}

	float Height = 0.0f;
	FVector Velocity = FVector::ZeroVector;
	const bool bHeightOk = Subsystem.GetFullWaveHeight(Location, Height);
	const bool bVelOk = Subsystem.GetWaterVelocity(Location, Velocity);

	if (!bHeightOk || !bVelOk)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("OceanVelDebug: point (%.0f, %.0f) is over no water body."),
			Location.X, Location.Y);
		bActive = false;
		bHavePrev = false;
		return;
	}

	if (bHavePrev && DeltaTime > KINDA_SMALL_NUMBER)
	{
		const float FiniteDiffZ = (Height - PrevHeight) / DeltaTime;
		UE_LOG(LogTemp, Log,
			TEXT("OceanVelDebug: VelZ = %8.1f cm/s | FD(Height) = %8.1f cm/s | diff = %6.1f | H = %.1f"),
			Velocity.Z, FiniteDiffZ, Velocity.Z - FiniteDiffZ, Height);
	}

	PrevHeight = Height;
	bHavePrev = true;
}

static FAutoConsoleCommandWithWorldAndArgs GOceanDebugWaterVelocityCmd(
	TEXT("Ocean.Debug.WaterVelocity"),
	TEXT("Log GetWaterVelocity vs finite-difference of GetFullWaveHeight at a fixed point 5m ahead of the camera. Args: [Seconds=5]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->GetSubsystem<UWaveParameterSubsystem>())
			{
				return;
			}

			const APlayerCameraManager* PCM =
				UGameplayStatics::GetPlayerCameraManager(World, 0);
			if (!PCM)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("Ocean.Debug.WaterVelocity: no player camera (run in PIE)."));
				return;
			}

			const float Seconds =
				Args.Num() > 0 ? FMath::Max(FCString::Atof(*Args[0]), 0.5f) : 5.0f;

			// Fixed point: 5 m ahead of the camera, flattened to XY —
			// height/velocity queries only use XY anyway.
			FVector Point = PCM->GetCameraLocation()
				+ PCM->GetCameraRotation().Vector() * 500.0f;

			OceanVelDebug::Location = Point;
			OceanVelDebug::Until = World->GetTimeSeconds() + Seconds;
			OceanVelDebug::bHavePrev = false;
			OceanVelDebug::bActive = true;

			UE_LOG(LogTemp, Log,
				TEXT("Ocean.Debug.WaterVelocity: logging at (%.0f, %.0f) for %.1fs."),
				Point.X, Point.Y, Seconds);
		}));

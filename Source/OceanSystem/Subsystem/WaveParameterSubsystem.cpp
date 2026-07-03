// Copyright James Joslin. All Rights Reserved.

#include "WaveParameterSubsystem.h"
#include "Components/SplineComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Misc/App.h"

DECLARE_STATS_GROUP(TEXT("OceanSystem"), STATGROUP_OceanSystem, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("WaveSubsystem Tick"), STAT_WaveSubsystemTick, STATGROUP_OceanSystem);
DECLARE_CYCLE_STAT(TEXT("WaveSubsystem Eval Physics"), STAT_WaveSubsystemEvalPhysics, STATGROUP_OceanSystem);
DECLARE_CYCLE_STAT(TEXT("WaveSubsystem Eval Full"), STAT_WaveSubsystemEvalFull, STATGROUP_OceanSystem);

// ---------------------------------------------------------------------------
// MID parameter name cache
// ---------------------------------------------------------------------------

namespace OceanMID
{
	static const FName WaveCountName(TEXT("WaveCount"));
	static const FName WaveTimeName(TEXT("WaveTime"));
	static const FName WaveDataTexName(TEXT("WaveDataTexture"));

	// Detail layers — per-pixel normal evaluation
	static const FName DetailWaveCountName(TEXT("DetailWaveCount"));
	static const FName DetailWaveDataTexName(TEXT("DetailWaveDataTexture"));
}

// ===================================================================
// Lifecycle
// ===================================================================

void UWaveParameterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	WaterBodies.Empty();
}

void UWaveParameterSubsystem::Deinitialize()
{
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

	const float WorldTime = World->GetTimeSeconds();
	const FWaterBodyQueryResult Query = QueryBodiesAt(WorldPos);

	if (!Query.IsValid())
	{
		return false;
	}

	if (Query.IsBlending())
	{
		// Blend zone — interpolate between primary and secondary configs
		const FWaterBodyEntry& Primary = WaterBodies[Query.PrimaryIndex];
		const FWaterBodyEntry& Secondary = WaterBodies[Query.SecondaryIndex];

		// For river bodies, use spline evaluator to get the correct BaseZ.
		// For flat bodies, use the stored BaseZ.
		// EvaluatePhysicsBlended handles flat/flat blending directly.
		// River/flat blend: evaluate each body independently and lerp.
		const FGerstnerResult ResultA = EvaluateBody(Primary, WorldPos, WorldTime, /*bFullEval=*/false);
		const FGerstnerResult ResultB = EvaluateBody(Secondary, WorldPos, WorldTime, /*bFullEval=*/false);

		const float Alpha = FMath::Clamp(Query.BlendAlpha, 0.0f, 1.0f);

		OutResult.Displacement = FMath::Lerp(ResultA.Displacement, ResultB.Displacement, Alpha);
		OutResult.Normal = FMath::Lerp(ResultA.Normal, ResultB.Normal, Alpha).GetSafeNormal();
		OutResult.WorldZ = FMath::Lerp(ResultA.WorldZ, ResultB.WorldZ, Alpha);
		OutResult.FoldIntensity = FMath::Lerp(ResultA.FoldIntensity, ResultB.FoldIntensity, Alpha);
	}
	else
	{
		// Single body — direct evaluation
		const FWaterBodyEntry& Entry = WaterBodies[Query.PrimaryIndex];
		OutResult = EvaluateBody(Entry, WorldPos, WorldTime, /*bFullEval=*/false);
	}

	return true;
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

	const float WorldTime = World->GetTimeSeconds();
	const FWaterBodyQueryResult Query = QueryBodiesAt(WorldPos);

	if (!Query.IsValid())
	{
		return false;
	}

	const FWaterBodyEntry& Entry = WaterBodies[Query.PrimaryIndex];
	const FGerstnerResult Result = EvaluateBody(Entry, WorldPos, WorldTime, /*bFullEval=*/true);
	OutWorldZ = Result.WorldZ;
	return true;
}

float UWaveParameterSubsystem::GetFoldIntensity(const FVector& WorldPos)
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0f;
	}

	const float WorldTime = World->GetTimeSeconds();
	const FWaterBodyQueryResult Query = QueryBodiesAt(WorldPos);

	if (!Query.IsValid())
	{
		return 0.0f;
	}

	const FWaterBodyEntry& Entry = WaterBodies[Query.PrimaryIndex];
	const FGerstnerResult Result = EvaluateBody(Entry, WorldPos, WorldTime, /*bFullEval=*/true);
	return Result.FoldIntensity;
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
			// Determine blend width: use the smaller of the two bodies' blend widths
			// (stored on the WaterBodyEntry; defaults set by the component).
			// For now, use a hardcoded default. Phase 4 components will populate
			// a BlendWidth field on the entry.
			constexpr float DefaultBlendWidth = 200.0f;

			FBlendZoneEntry BZ;
			BZ.BodyA = NewEntry.Owner;
			BZ.BodyB = Other.Owner;
			BZ.BlendWidth = DefaultBlendWidth;
			BZ.BlendType = EBlendType::DepthFade;

			NewEntry.BlendZones.Add(BZ);
			Other.BlendZones.Add(BZ);
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
			// River: domain warp the position, then evaluate along spline.
			// CrestSharpness applied via the internal loop.
			const float ScaledTime = WorldTime * Entry.WaveConfig.TimeScale;
			const FVector WarpedPos = (Entry.DomainWarpAmount > UE_KINDA_SMALL_NUMBER)
				? FGerstnerEvaluator::DomainWarpPosition(
					WorldPos, ScaledTime,
					Entry.DomainWarpFrequency, Entry.DomainWarpAmount)
				: WorldPos;

			// Spline evaluators don't have a visual variant yet —
			// use standard eval at the warped position.
			return bFullEval
				? FGerstnerEvaluator::EvaluateAlongSpline(WarpedPos, WorldTime, Spline, Entry.WaveConfig)
				: FGerstnerEvaluator::EvaluatePhysicsAlongSpline(WarpedPos, WorldTime, Spline, Entry.WaveConfig);
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
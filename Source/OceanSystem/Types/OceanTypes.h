// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Math/Box2D.h"
#include "OceanTypes.generated.h"

// Forward declarations for Phase 4+ types used by TWeakObjectPtr
class UOceanBodyComponent;
class USplineComponent;
class UMaterialInstanceDynamic;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

UENUM(BlueprintType)
enum class EOceanBodyType : uint8
{
	Ocean,
	Lake,
	River
};

UENUM(BlueprintType)
enum class EBlendType : uint8
{
	/** Fade based on depth below overlapping body's surface. */
	DepthFade,
	/** Alpha overlap � both bodies render, opacity blended. */
	AlphaOverlap
};

// ---------------------------------------------------------------------------
// FGerstnerWaveLayer
// ---------------------------------------------------------------------------

/**
 * Single Gerstner wave layer.
 *
 * All spatial values are in UE world units (centimetres).
 * Direction must be normalised. Steepness (Q) in [0,1] controls crest
 * sharpness — 0 is a pure sine, 1 is the sharpest crest before looping.
 * Speed is phase velocity in cm/s; PhaseOffset adds a constant phase shift
 * so multiple layers with the same direction don't peak together.
 */
USTRUCT(BlueprintType)
struct OCEANSYSTEM_API FGerstnerWaveLayer
{
	GENERATED_BODY()

	/** Wave propagation direction (normalised XY). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	FVector2D Direction = FVector2D(1.0, 0.0);

	/** Peak amplitude in cm. 100 = 1 metre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave", meta = (ClampMin = "0.1", UIMin = "1.0", UIMax = "1000.0"))
	float Amplitude = 100.0f;

	/** Wavelength in cm (peak-to-peak). 10000 = 100 metres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave", meta = (ClampMin = "10.0", UIMin = "50.0", UIMax = "50000.0"))
	float Wavelength = 4000.0f;

	/** Steepness Q in [0,1]. Controls crest sharpness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Steepness = 0.5f;

	/** Phase velocity in cm/s. Derived from wavelength via dispersion relation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave", meta = (ClampMin = "0.0", UIMin = "10.0", UIMax = "3000.0"))
	float Speed = 500.0f;

	/** Constant phase offset in radians. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	float PhaseOffset = 0.0f;
};

// ---------------------------------------------------------------------------
// FWaveConfig
// ---------------------------------------------------------------------------

/**
 * Complete wave configuration for a single water body.
 *
 * Layers are kept sorted by amplitude descending so that the first N layers
 * are always the physically dominant ones. PhysicsLayerCount controls how
 * many layers the CPU evaluator processes for buoyancy � detail ripples are
 * skipped on the physics path.
 *
 * Call SortLayers() after modifying the Layers array directly.
 * SetWaveConfig() on OceanBodyComponent handles this automatically.
 */
USTRUCT(BlueprintType)
struct OCEANSYSTEM_API FWaveConfig
{
	GENERATED_BODY()

	static constexpr int32 MaxLayers = 16;

	/**
	 * Wave layers, sorted by amplitude descending.
	 * Maximum 16 layers.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Waves", meta = (TitleProperty = "Amplitude"))
	TArray<FGerstnerWaveLayer> Layers;

	/**
	 * Number of layers evaluated on CPU for buoyancy physics.
	 * The first PhysicsLayerCount layers (the largest by amplitude) are used.
	 * Remaining layers are GPU-only visual detail.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "8"))
	int32 PhysicsLayerCount = 3;

	/** Global time scale multiplier for this body's waves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Waves", meta = (ClampMin = "0.0", UIMin = "0.1", UIMax = "3.0"))
	float TimeScale = 1.0f;

	/** Set by OceanBodyComponent when config changes; cleared after MID sync. */
	bool bDirty = false;

	// -------------------------------------------------------------------
	// Methods
	// -------------------------------------------------------------------

	/** Sort layers by amplitude descending. Call after modifying Layers directly. */
	void SortLayers()
	{
		Layers.Sort([](const FGerstnerWaveLayer& A, const FGerstnerWaveLayer& B)
			{
				return A.Amplitude > B.Amplitude;
			});
	}

	/** Effective physics layer count, clamped to actual layer count. */
	int32 GetPhysicsLayerCount() const
	{
		return FMath::Clamp(PhysicsLayerCount, 0, Layers.Num());
	}

	//int32 GetVisualLayerCount() const
	//{
	//	return FMath::Min(Layers.Num(), MaxLayers);
	//}

	/** Total layer count for CPU full evaluation (spray, debug). */
	int32 GetTotalLayerCount() const
	{
		return FMath::Min(Layers.Num(), MaxLayers);
	}

	/** Add a layer, re-sort, and enforce MaxLayers cap. */
	void AddLayer(const FGerstnerWaveLayer& Layer)
	{
		if (Layers.Num() < MaxLayers)
		{
			Layers.Add(Layer);
			SortLayers();
			bDirty = true;
		}
	}

	/** Remove all layers. */
	void ClearLayers()
	{
		Layers.Empty();
		bDirty = true;
	}

	/** Build a sensible default 4-layer ocean config. */
	static FWaveConfig MakeDefaultOcean()
	{
		FWaveConfig Config;
		Config.PhysicsLayerCount = 3;
		Config.TimeScale = 1.0f;

		FGerstnerWaveLayer L0;
		L0.Direction = FVector2D(0.9f, 0.4f).GetSafeNormal();
		L0.Amplitude = 250.0f;
		L0.Wavelength = 10000.0f;
		L0.Steepness = 0.6f;
		L0.Speed = 800.0f;
		L0.PhaseOffset = 0.0f;
		Config.Layers.Add(L0);

		FGerstnerWaveLayer L1;
		L1.Direction = FVector2D(0.6f, -0.8f).GetSafeNormal();
		L1.Amplitude = 150.0f;
		L1.Wavelength = 6000.0f;
		L1.Steepness = 0.5f;
		L1.Speed = 600.0f;
		L1.PhaseOffset = 1.2f;
		Config.Layers.Add(L1);

		FGerstnerWaveLayer L2;
		L2.Direction = FVector2D(-0.3f, 0.95f).GetSafeNormal();
		L2.Amplitude = 80.0f;
		L2.Wavelength = 3000.0f;
		L2.Steepness = 0.4f;
		L2.Speed = 450.0f;
		L2.PhaseOffset = 2.7f;
		Config.Layers.Add(L2);

		FGerstnerWaveLayer L3;
		L3.Direction = FVector2D(0.2f, 0.98f).GetSafeNormal();
		L3.Amplitude = 30.0f;
		L3.Wavelength = 1200.0f;
		L3.Steepness = 0.35f;
		L3.Speed = 300.0f;
		L3.PhaseOffset = 0.8f;
		Config.Layers.Add(L3);

		Config.SortLayers();
		return Config;
	}
};

// ---------------------------------------------------------------------------
// FBlendZoneEntry
// ---------------------------------------------------------------------------

/**
 * Defines a blend zone between two overlapping water bodies.
 * Auto-created by WaveParameterSubsystem when body bounds overlap.
 */
USTRUCT()
struct FBlendZoneEntry
{
	GENERATED_BODY()

	/** First water body in the blend pair. */
	TWeakObjectPtr<UOceanBodyComponent> BodyA;

	/** Second water body in the blend pair. */
	TWeakObjectPtr<UOceanBodyComponent> BodyB;

	/** Width of the blend region in world units. */
	float BlendWidth = 200.0f;

	/** How the blend is rendered. */
	EBlendType BlendType = EBlendType::DepthFade;
};

// ---------------------------------------------------------------------------
// FWaterBodyEntry � Subsystem-internal registry record
// ---------------------------------------------------------------------------

/**
 * Internal bookkeeping struct held by WaveParameterSubsystem per registered
 * water body. Not intended for external use � treat as opaque.
 */
USTRUCT()
struct FWaterBodyEntry
{
	GENERATED_BODY()

	/** Owning component (source of truth for config edits). */
	TWeakObjectPtr<UOceanBodyComponent> Owner;

	/** Cached body type for fast dispatch. */
	EOceanBodyType BodyType = EOceanBodyType::Ocean;

	/** XY bounds for spatial queries (ocean/lake only). */
	FBox2D Bounds = FBox2D(ForceInit);

	/** Resting water surface height (ocean/lake). */
	float BaseZ = 0.0f;

	/** Overlap resolution � higher priority body wins queries. */
	int32 Priority = 0;

	/** Cached wave config (copied on register and on dirty). */
	FWaveConfig WaveConfig;

	/** Detail wave config — short-wavelength layers for per-pixel normals only.
		Not evaluated on CPU. Populated by the wave generator. */
	FWaveConfig DetailWaveConfig;

	/** Spline reference for river gradient queries. Null for ocean/lake. */
	TWeakObjectPtr<USplineComponent> SplineData;

	/** Half-width of the river cross-section in world units. */
	float RiverHalfWidth = 0.0f;

	/** Dynamic material instance for MID parameter sync. */
	TWeakObjectPtr<UMaterialInstanceDynamic> MaterialInstance;

	/** Runtime wave data texture for GPU lookup. Created/owned by subsystem. */
	TWeakObjectPtr<UTexture2D> WaveDataTexture;

	/** Runtime detail wave data texture for GPU lookup. Created/owned by subsystem. */
	TWeakObjectPtr<UTexture2D> DetailWaveDataTexture;

	/** Active blend zones involving this body. */
	TArray<FBlendZoneEntry> BlendZones;

	// -------------------------------------------------------------------
	// Visual parameters (GPU-only effects replicated on CPU for parity)
	// -------------------------------------------------------------------

	/** Domain warp frequency — bends straight crests into organic curves. */
	float DomainWarpFrequency = 0.00035f;

	/** Domain warp displacement amount in world units. */
	float DomainWarpAmount = 400.0f;

	/** Crest sharpness — asymmetric crest/trough shaping. 1.0 = standard sine. */
	float CrestSharpness = 1.5f;
};
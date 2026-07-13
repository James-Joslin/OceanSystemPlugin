// Copyright James Joslin. All Rights Reserved.

#include "OceanWaterBodyActor.h"
#include "../Components/OceanBodyComponent.h"
#include "../Components/TiledWaterMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "../Components/UnderwaterPostProcessComponent.h"

AOceanWaterBodyActor::AOceanWaterBodyActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// --- OceanBody (root) ---
	OceanBody = CreateDefaultSubobject<UOceanBodyComponent>(TEXT("OceanBody"));
	RootComponent = OceanBody;
	OceanBody->BodyType = EOceanBodyType::Ocean;
	OceanBody->Priority = 0;

	// --- Wave generator defaults for open ocean (all spatial values in cm) ---
	// Large swells: low steepness (broad rolling motion).
	// Small chop:   high steepness (sharp peaked crests).
	// The contrast between the two is what gives the ocean shape.
	FWaveGeneratorConfig& Gen = OceanBody->WaveGenerator;
	Gen.NumWaves = 12;
	Gen.Seed = 0;
	Gen.Randomness = 0.25f;
	Gen.MinWavelength = 500.0f;
	Gen.MaxWavelength = 12000.0f;
	Gen.WavelengthFalloff = 2.5f;
	Gen.MinAmplitude = 5.0f;
	Gen.MaxAmplitude = 300.0f;
	Gen.AmplitudeFalloff = 3.0f;
	Gen.LargeWaveSteepness = 0.08f;
	Gen.SmallWaveSteepness = 0.65f;
	Gen.SteepnessFalloff = 2.0f;
	Gen.DominantWindAngle = 0.0f;
	Gen.DirectionAngularSpread = 80.0f;
	Gen.GlobalSpeedMultiplier = 1.0f;
	Gen.NoiseStrength = 0.3f;
	Gen.NoiseOctaves = 3;
	Gen.NoiseLacunarity = 2.0f;
	Gen.NoiseGain = 0.5f;
	Gen.NoiseWarpStrength = 0.5f;
	Gen.PhysicsLayerCount = 3;
	Gen.TimeScale = 1.0f;

	// Generate initial wave config from generator
	OceanBody->WaveConfig = Gen.Generate();

	// --- Detail wave generator defaults for per-pixel normal chop ---
	// Short wavelengths, higher steepness. These layers never displace
	// geometry — they only perturb the surface normal per-pixel, giving
	// crest sharpness that the mesh resolution can't resolve.
	FWaveGeneratorConfig& Detail = OceanBody->DetailWaveGenerator;
	Detail.NumWaves = 8;
	Detail.Seed = 100;      // decorrelate from main
	Detail.Randomness = 0.3f;
	Detail.MinWavelength = 50.0f;    // 0.5m — capillary scale
	Detail.MaxWavelength = 800.0f;   // 8m — fills gap between swell and normal maps
	Detail.WavelengthFalloff = 1.5f;
	Detail.MinAmplitude = 1.0f;     // tiny — normal perturbation only
	Detail.MaxAmplitude = 15.0f;
	Detail.AmplitudeFalloff = 1.5f;
	Detail.LargeWaveSteepness = 0.4f;     // steeper than swell — visible crests
	Detail.SmallWaveSteepness = 0.7f;
	Detail.SteepnessFalloff = 1.0f;
	Detail.DominantWindAngle = 0.0f;     // match main wind
	Detail.DirectionAngularSpread = 160.0f;  // wide spread for choppy look
	Detail.GlobalSpeedMultiplier = 0.6f;     // detail waves travel slower
	Detail.NoiseStrength = 0.4f;
	Detail.NoiseOctaves = 2;
	Detail.NoiseLacunarity = 2.0f;
	Detail.NoiseGain = 0.5f;
	Detail.NoiseWarpStrength = 0.4f;
	Detail.PhysicsLayerCount = 0;        // never evaluated on CPU
	Detail.TimeScale = 1.0f;

	OceanBody->DetailWaveConfig = Detail.Generate();

	// --- Tiled ocean mesh ---
	TiledMesh = CreateDefaultSubobject<UTiledWaterMeshComponent>(TEXT("TiledMesh"));
	TiledMesh->SetupAttachment(OceanBody);

	// Ocean-scale tile grid: 8x8 tiles at 32768 cm each = 2.6km x 2.6km coverage.
	// 64 tiles vs 256 — 4x faster PIE startup.
	// LOD0 cell size = 32768/64 = 512cm — resolves waves >= ~2000cm.
	// Detail below that comes from material-side normals (future work).
	TiledMesh->TilesX = 8;
	TiledMesh->TilesY = 8;
	TiledMesh->TileSize = 32768.0f;
	TiledMesh->TileSubdivisions = 64;
	TiledMesh->LODDistances = { 16000.0f, 40000.0f, 80000.0f };
	TiledMesh->LODSubdivisions = { 32, 16, 8 };
}

// ===================================================================
// Transform lock — the tiled mesh always shares the root's transform
// ===================================================================
//
// The subsystem registers the physics water level (BaseZ) from the
// root OceanBody component. If the mesh drifts relative to the root,
// the rendered surface and the physics surface silently diverge and
// buoyancy objects float in the air or sink through the visuals.
// Water is placed by moving the ACTOR, never the mesh child.
// ===================================================================

void AOceanWaterBodyActor::EnforceMeshTransformLock()
{
	if (!TiledMesh)
	{
		return;
	}

	if (!TiledMesh->GetRelativeTransform().Equals(FTransform::Identity, 0.1f))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("OceanWaterBodyActor '%s': TiledMesh had a relative offset "
				"(%.1f, %.1f, %.1f) — snapped back to the root. Move the actor "
				"itself to reposition the water."),
			*GetName(),
			TiledMesh->GetRelativeLocation().X,
			TiledMesh->GetRelativeLocation().Y,
			TiledMesh->GetRelativeLocation().Z);

		TiledMesh->SetRelativeTransform(FTransform::Identity);
	}
}

void AOceanWaterBodyActor::BeginPlay()
{
	Super::BeginPlay();
	EnforceMeshTransformLock();
	SyncExtentFromMesh();
	RefreshMeshMaterial();
	UpdateBoundsFromWaveConfig();
}

#if WITH_EDITOR
void AOceanWaterBodyActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!OceanBody || !TiledMesh)
	{
		return;
	}

	EnforceMeshTransformLock();
	SyncExtentFromMesh();

	// Ensure MID exists and body is registered with subsystem.
	// Skips MID recreation if parent material hasn't changed.
	OceanBody->InitializeWaterBody();

	// Build tiles only on first construction — not on every property change.
	// Grid property changes (TilesX, TilesY, TileSize, TileSubdivisions) are
	// handled by TiledWaterMeshComponent::PostEditChangeProperty.
	if (TiledMesh->GetTileCount() == 0)
	{
		TiledMesh->BuildTileMesh();
	}

	// Apply MID to tiles so wave parameters drive the shader.
	// Fall back to base material if MID creation failed.
	UMaterialInstanceDynamic* MID = OceanBody->GetMaterialInstance();
	if (MID)
	{
		TiledMesh->SetMaterialOnAllTiles(MID);
	}
	else
	{
		UMaterialInterface* BaseMat = OceanBody->BaseMaterial.Get();
		if (BaseMat)
		{
			TiledMesh->SetMaterialOnAllTiles(BaseMat);
		}
	}
}
#endif

void AOceanWaterBodyActor::RefreshMeshMaterial()
{
	if (!OceanBody || !TiledMesh)
	{
		return;
	}

	UMaterialInstanceDynamic* MID = OceanBody->GetMaterialInstance();
	if (!MID)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("OceanWaterBodyActor '%s': No MID — set BaseMaterial on OceanBody."),
			*GetName());
		return;
	}

	TiledMesh->SetMaterialOnAllTiles(MID);
}

void AOceanWaterBodyActor::UpdateBoundsFromWaveConfig()
{
	if (!OceanBody || !TiledMesh)
	{
		return;
	}

	float MaxDisplacement = 0.0f;
	for (const FGerstnerWaveLayer& Layer : OceanBody->WaveConfig.Layers)
	{
		MaxDisplacement += Layer.Amplitude;
	}

	TiledMesh->VerticalBoundsExtension = MaxDisplacement * 1.5f;
}

void AOceanWaterBodyActor::SyncExtentFromMesh()
{
	if (!OceanBody || !TiledMesh)
	{
		return;
	}

	OceanBody->Extent = FVector2D(
		TiledMesh->TilesX * TiledMesh->TileSize * 0.5,
		TiledMesh->TilesY * TiledMesh->TileSize * 0.5);

	OceanBody->InitializeWaterBody();
}
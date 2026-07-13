// Copyright James Joslin. All Rights Reserved.

#include "LakeWaterBodyActor.h"
#include "../Components/OceanBodyComponent.h"
#include "../Components/TiledWaterMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "../Components/UnderwaterPostProcessComponent.h"

ALakeWaterBodyActor::ALakeWaterBodyActor()
{
	PrimaryActorTick.bCanEverTick = false;

	OceanBody = CreateDefaultSubobject<UOceanBodyComponent>(TEXT("OceanBody"));
	RootComponent = OceanBody;
	OceanBody->BodyType = EOceanBodyType::Lake;
	OceanBody->Priority = 10;

	// --- Wave generator defaults for a calm lake (all spatial values in cm) ---
	FWaveGeneratorConfig& Gen = OceanBody->WaveGenerator;
	Gen.NumWaves = 8;
	Gen.Seed = 42;
	Gen.Randomness = 0.15f;
	Gen.MinWavelength = 300.0f;
	Gen.MaxWavelength = 2500.0f;
	Gen.WavelengthFalloff = 1.5f;
	Gen.MinAmplitude = 1.0f;
	Gen.MaxAmplitude = 40.0f;
	Gen.AmplitudeFalloff = 2.0f;
	Gen.LargeWaveSteepness = 0.2f;
	Gen.SmallWaveSteepness = 0.35f;
	Gen.SteepnessFalloff = 1.0f;
	Gen.DominantWindAngle = 45.0f;
	Gen.DirectionAngularSpread = 90.0f;
	Gen.GlobalSpeedMultiplier = 0.8f;
	Gen.NoiseStrength = 0.2f;
	Gen.NoiseOctaves = 2;
	Gen.NoiseLacunarity = 2.0f;
	Gen.NoiseGain = 0.5f;
	Gen.NoiseWarpStrength = 0.3f;
	Gen.PhysicsLayerCount = 2;
	Gen.TimeScale = 1.0f;

	OceanBody->WaveConfig = Gen.Generate();

	// --- Detail wave generator defaults for lake surface chop ---
	// Gentler than ocean — smaller amplitudes, narrower spread.
	FWaveGeneratorConfig& Detail = OceanBody->DetailWaveGenerator;
	Detail.NumWaves = 6;
	Detail.Seed = 142;      // decorrelate from main
	Detail.Randomness = 0.2f;
	Detail.MinWavelength = 40.0f;    // fine ripples
	Detail.MaxWavelength = 500.0f;
	Detail.WavelengthFalloff = 1.5f;
	Detail.MinAmplitude = 0.5f;
	Detail.MaxAmplitude = 5.0f;
	Detail.AmplitudeFalloff = 1.5f;
	Detail.LargeWaveSteepness = 0.25f;
	Detail.SmallWaveSteepness = 0.45f;
	Detail.SteepnessFalloff = 1.0f;
	Detail.DominantWindAngle = 45.0f;    // match main
	Detail.DirectionAngularSpread = 140.0f;
	Detail.GlobalSpeedMultiplier = 0.5f;
	Detail.NoiseStrength = 0.3f;
	Detail.NoiseOctaves = 2;
	Detail.NoiseLacunarity = 2.0f;
	Detail.NoiseGain = 0.5f;
	Detail.NoiseWarpStrength = 0.3f;
	Detail.PhysicsLayerCount = 0;        // never on CPU
	Detail.TimeScale = 1.0f;

	OceanBody->DetailWaveConfig = Detail.Generate();

	TiledMesh = CreateDefaultSubobject<UTiledWaterMeshComponent>(TEXT("TiledMesh"));
	TiledMesh->SetupAttachment(OceanBody);
}

// ===================================================================
// Transform lock — the tiled mesh always shares the root's transform
// (see OceanWaterBodyActor for rationale)
// ===================================================================

void ALakeWaterBodyActor::EnforceMeshTransformLock()
{
	if (!TiledMesh)
	{
		return;
	}

	if (!TiledMesh->GetRelativeTransform().Equals(FTransform::Identity, 0.1f))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("LakeWaterBodyActor '%s': TiledMesh had a relative offset "
				"(%.1f, %.1f, %.1f) — snapped back to the root. Move the actor "
				"itself to reposition the water."),
			*GetName(),
			TiledMesh->GetRelativeLocation().X,
			TiledMesh->GetRelativeLocation().Y,
			TiledMesh->GetRelativeLocation().Z);

		TiledMesh->SetRelativeTransform(FTransform::Identity);
	}
}

void ALakeWaterBodyActor::BeginPlay()
{
	Super::BeginPlay();
	EnforceMeshTransformLock();
	SyncExtentFromMesh();
	RefreshMeshMaterial();
	UpdateBoundsFromWaveConfig();
}

#if WITH_EDITOR
void ALakeWaterBodyActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!OceanBody || !TiledMesh)
	{
		return;
	}

	EnforceMeshTransformLock();
	SyncExtentFromMesh();

	// Ensure MID exists and body is registered with subsystem.
	OceanBody->InitializeWaterBody();

	// Build tiles only on first construction.
	if (TiledMesh->GetTileCount() == 0)
	{
		TiledMesh->BuildTileMesh();
	}

	// Apply MID to tiles.
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

void ALakeWaterBodyActor::RefreshMeshMaterial()
{
	if (!OceanBody || !TiledMesh) return;

	UMaterialInstanceDynamic* MID = OceanBody->GetMaterialInstance();
	if (!MID)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("LakeWaterBodyActor '%s': No MID — set BaseMaterial on OceanBody."),
			*GetName());
		return;
	}

	TiledMesh->SetMaterialOnAllTiles(MID);
}

void ALakeWaterBodyActor::UpdateBoundsFromWaveConfig()
{
	if (!OceanBody || !TiledMesh) return;

	float MaxDisp = 0.0f;
	for (const FGerstnerWaveLayer& Layer : OceanBody->WaveConfig.Layers)
	{
		MaxDisp += Layer.Amplitude;
	}

	TiledMesh->VerticalBoundsExtension = MaxDisp * 1.5f;
}

void ALakeWaterBodyActor::SyncExtentFromMesh()
{
	if (!OceanBody || !TiledMesh) return;

	OceanBody->Extent = FVector2D(
		TiledMesh->TilesX * TiledMesh->TileSize * 0.5,
		TiledMesh->TilesY * TiledMesh->TileSize * 0.5);

	OceanBody->InitializeWaterBody();
}
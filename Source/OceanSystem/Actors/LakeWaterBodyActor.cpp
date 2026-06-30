// Copyright James Joslin. All Rights Reserved.

#include "LakeWaterBodyActor.h"
#include "../Components/OceanBodyComponent.h"
#include "../Components/TiledWaterMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

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

	TiledMesh = CreateDefaultSubobject<UTiledWaterMeshComponent>(TEXT("TiledMesh"));
	TiledMesh->SetupAttachment(OceanBody);
}

void ALakeWaterBodyActor::BeginPlay()
{
	Super::BeginPlay();
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
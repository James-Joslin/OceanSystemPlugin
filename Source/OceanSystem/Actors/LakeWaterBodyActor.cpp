// Copyright James Joslin. All Rights Reserved.

#include "LakeWaterBodyActor.h"
#include "../Components/OceanBodyComponent.h"
#include "../Components/TiledWaterMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

// ===================================================================
// Constructor
// ===================================================================

ALakeWaterBodyActor::ALakeWaterBodyActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// --- OceanBody (root) ---
	OceanBody = CreateDefaultSubobject<UOceanBodyComponent>(TEXT("OceanBody"));
	RootComponent = OceanBody;
	OceanBody->BodyType = EOceanBodyType::Lake;
	OceanBody->Priority = 10;

	// Lakes typically have calmer waves — override the ocean default.
	// The user can further customise in the Details panel.
	FWaveConfig LakeConfig;
	LakeConfig.PhysicsLayerCount = 2;
	LakeConfig.TimeScale = 1.0f;

	FGerstnerWaveLayer L0;
	L0.Direction = FVector2D(0.8f, 0.6f).GetSafeNormal();
	L0.Amplitude = 0.4f;
	L0.Wavelength = 25.0f;
	L0.Steepness = 0.3f;
	L0.Speed = 2.5f;
	L0.PhaseOffset = 0.0f;
	LakeConfig.Layers.Add(L0);

	FGerstnerWaveLayer L1;
	L1.Direction = FVector2D(-0.5f, 0.85f).GetSafeNormal();
	L1.Amplitude = 0.2f;
	L1.Wavelength = 12.0f;
	L1.Steepness = 0.25f;
	L1.Speed = 1.8f;
	L1.PhaseOffset = 1.5f;
	LakeConfig.Layers.Add(L1);

	FGerstnerWaveLayer L2;
	L2.Direction = FVector2D(0.3f, -0.95f).GetSafeNormal();
	L2.Amplitude = 0.08f;
	L2.Wavelength = 5.0f;
	L2.Steepness = 0.2f;
	L2.Speed = 1.0f;
	L2.PhaseOffset = 3.1f;
	LakeConfig.Layers.Add(L2);

	LakeConfig.SortLayers();
	OceanBody->WaveConfig = LakeConfig;

	// --- Tiled mesh ---
	TiledMesh = CreateDefaultSubobject<UTiledWaterMeshComponent>(TEXT("TiledMesh"));
	TiledMesh->SetupAttachment(OceanBody);

	// Step 16: UnderwaterPP = CreateDefaultSubobject<UUnderwaterPostProcessComponent>(...)
}

// ===================================================================
// Lifecycle
// ===================================================================

void ALakeWaterBodyActor::BeginPlay()
{
	Super::BeginPlay();

	// Components have already had their BeginPlay called:
	//   OceanBody → MID created, registered with subsystem
	//   TiledMesh → tile meshes built with all LOD sections

	RefreshMeshMaterial();
	UpdateBoundsFromWaveConfig();
}

#if WITH_EDITOR
void ALakeWaterBodyActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Editor preview: apply the base material to tile sections
	if (OceanBody && TiledMesh)
	{
		UMaterialInterface* BaseMat = OceanBody->BaseMaterial.Get();
		if (BaseMat)
		{
			TiledMesh->SetMaterialOnAllTiles(BaseMat);
		}
	}
}
#endif

// ===================================================================
// RefreshMeshMaterial
// ===================================================================

void ALakeWaterBodyActor::RefreshMeshMaterial()
{
	if (!OceanBody || !TiledMesh)
	{
		return;
	}

	UMaterialInstanceDynamic* MID = OceanBody->GetMaterialInstance();
	if (!MID)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("LakeWaterBodyActor '%s': No MID available — tiles will use default material. "
				"Ensure BaseMaterial is set on the OceanBody component."),
			*GetName());
		return;
	}

	TiledMesh->SetMaterialOnAllTiles(MID);
}

// ===================================================================
// UpdateBoundsFromWaveConfig
// ===================================================================

void ALakeWaterBodyActor::UpdateBoundsFromWaveConfig()
{
	if (!OceanBody || !TiledMesh)
	{
		return;
	}

	const FWaveConfig& Config = OceanBody->WaveConfig;

	float MaxDisplacement = 0.0f;
	for (const FGerstnerWaveLayer& Layer : Config.Layers)
	{
		MaxDisplacement += Layer.Amplitude;
	}

	TiledMesh->VerticalBoundsExtension = MaxDisplacement * 1.5f;
}
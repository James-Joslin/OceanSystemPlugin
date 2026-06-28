// Copyright James Joslin. All Rights Reserved.

#include "OceanWaterBodyActor.h"
#include "../Components/OceanBodyComponent.h"
#include "../Components/ClipmapOceanMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

// ===================================================================
// Constructor
// ===================================================================

AOceanWaterBodyActor::AOceanWaterBodyActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// --- OceanBody (root) ---
	OceanBody = CreateDefaultSubobject<UOceanBodyComponent>(TEXT("OceanBody"));
	RootComponent = OceanBody;
	OceanBody->BodyType = EOceanBodyType::Ocean;
	OceanBody->Priority = 0;

	// --- Clipmap mesh ---
	ClipmapMesh = CreateDefaultSubobject<UClipmapOceanMeshComponent>(TEXT("ClipmapMesh"));
	ClipmapMesh->SetupAttachment(OceanBody);

	// Step 16: UnderwaterPP = CreateDefaultSubobject<UUnderwaterPostProcessComponent>(...)
	// Step 18: OceanSpray   = CreateDefaultSubobject<UOceanSprayComponent>(...)
}

// ===================================================================
// Lifecycle
// ===================================================================

void AOceanWaterBodyActor::BeginPlay()
{
	Super::BeginPlay();

	// Components have already had their BeginPlay called at this point:
	//   OceanBody  → MID created, registered with subsystem
	//   ClipmapMesh → mesh sections built

	RefreshMeshMaterial();
	UpdateBoundsFromWaveConfig();
}

#if WITH_EDITOR
void AOceanWaterBodyActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Editor preview: apply the base material (not MID) to any existing
	// mesh sections so the surface is visible outside PIE. The MID with
	// animated wave parameters is applied at BeginPlay instead.
	if (OceanBody && ClipmapMesh)
	{
		UMaterialInterface* BaseMat = OceanBody->BaseMaterial.Get();
		if (BaseMat)
		{
			for (int32 i = 0; i < ClipmapMesh->RingCount; ++i)
			{
				ClipmapMesh->SetMaterial(i, BaseMat);
			}
		}
	}
}
#endif

// ===================================================================
// RefreshMeshMaterial
// ===================================================================

void AOceanWaterBodyActor::RefreshMeshMaterial()
{
	if (!OceanBody || !ClipmapMesh)
	{
		return;
	}

	UMaterialInstanceDynamic* MID = OceanBody->GetMaterialInstance();
	if (!MID)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("OceanWaterBodyActor '%s': No MID available — mesh will use default material. "
				"Ensure BaseMaterial is set on the OceanBody component."),
			*GetName());
		return;
	}

	for (int32 i = 0; i < ClipmapMesh->RingCount; ++i)
	{
		ClipmapMesh->SetMaterial(i, MID);
	}
}

// ===================================================================
// UpdateBoundsFromWaveConfig
// ===================================================================

void AOceanWaterBodyActor::UpdateBoundsFromWaveConfig()
{
	if (!OceanBody || !ClipmapMesh)
	{
		return;
	}

	const FWaveConfig& Config = OceanBody->WaveConfig;

	// Compute the theoretical maximum vertical displacement.
	// Gerstner Z displacement = Σ A_i × sin(θ_i). Worst case is all
	// layers peaking simultaneously (unlikely with varied directions
	// and wavelengths, but a safe upper bound).
	float MaxDisplacement = 0.0f;
	for (const FGerstnerWaveLayer& Layer : Config.Layers)
	{
		MaxDisplacement += Layer.Amplitude;
	}

	// Add 50% margin — horizontal displacement (Q × A) shifts geometry
	// sideways, effectively extending the visible surface beyond the
	// un-displaced bounds.
	ClipmapMesh->VerticalBoundsExtension = MaxDisplacement * 1.5f;
}
// Copyright James Joslin. All Rights Reserved.

#include "OceanBodyComponent.h"
#include "../Subsystem/WaveParameterSubsystem.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"

UOceanBodyComponent::UOceanBodyComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	WaveConfig = FWaveConfig::MakeDefaultOcean();
}

// ===================================================================
// Lifecycle
// ===================================================================

void UOceanBodyComponent::BeginPlay()
{
	Super::BeginPlay();

	// --- Create Dynamic Material Instance ---
	UMaterialInterface* BaseMat = BaseMaterial.LoadSynchronous();
	if (BaseMat)
	{
		MaterialInstance = UMaterialInstanceDynamic::Create(BaseMat, this,
			FName(*FString::Printf(TEXT("MID_%s"), *GetOwner()->GetName())));
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("OceanBodyComponent on '%s': BaseMaterial is not set — no MID created. "
				"Waves will evaluate on CPU but the mesh will not animate."),
			*GetOwner()->GetName());
	}

	// --- Register with subsystem ---
	if (UWorld* World = GetWorld())
	{
		if (UWaveParameterSubsystem* Subsystem = World->GetSubsystem<UWaveParameterSubsystem>())
		{
			FWaterBodyEntry Entry = BuildRegistryEntry();
			Subsystem->RegisterWaterBody(Entry);
		}
	}
}

void UOceanBodyComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (UWaveParameterSubsystem* Subsystem = World->GetSubsystem<UWaveParameterSubsystem>())
		{
			Subsystem->UnregisterWaterBody(this);
		}
	}

	MaterialInstance = nullptr;
	Super::EndPlay(EndPlayReason);
}

// ===================================================================
// Runtime API
// ===================================================================

void UOceanBodyComponent::SetWaveConfig(const FWaveConfig& NewConfig)
{
	WaveConfig = NewConfig;
	WaveConfig.SortLayers();
	WaveConfig.bDirty = true;

	if (UWorld* World = GetWorld())
	{
		if (UWaveParameterSubsystem* Subsystem = World->GetSubsystem<UWaveParameterSubsystem>())
		{
			Subsystem->UpdateWaterBodyConfig(this, WaveConfig);
		}
	}
}

// ===================================================================
// Internal
// ===================================================================

FWaterBodyEntry UOceanBodyComponent::BuildRegistryEntry() const
{
	FWaterBodyEntry Entry;

	Entry.Owner = const_cast<UOceanBodyComponent*>(this);
	Entry.BodyType = BodyType;
	Entry.Priority = Priority;

	// Wave config — already sorted by MakeDefaultOcean or prior SetWaveConfig calls,
	// but the subsystem re-sorts on registration as a safety net.
	Entry.WaveConfig = WaveConfig;
	Entry.WaveConfig.bDirty = true;  // Force initial MID sync

	// MID
	Entry.MaterialInstance = MaterialInstance;

	// Bounds and BaseZ from world transform
	const FVector WorldPos = GetComponentLocation();
	Entry.BaseZ = WorldPos.Z;

	switch (BodyType)
	{
	case EOceanBodyType::Ocean:
	case EOceanBodyType::Lake:
	{
		const FVector2D Centre(WorldPos.X, WorldPos.Y);
		Entry.Bounds = FBox2D(Centre - Extent, Centre + Extent);
		break;
	}
	case EOceanBodyType::River:
		// River bounds are derived from the spline in the actor.
		// SplineData and RiverHalfWidth are set by RiverWaterBodyActor
		// before calling RegisterWaterBody. Leave defaults here.
		Entry.RiverHalfWidth = 0.0f;
		break;
	}

	// Spline data is not set here — river actors populate it directly
	// on the entry before or after registration.
	// BlendZones are auto-detected by the subsystem.

	return Entry;
}
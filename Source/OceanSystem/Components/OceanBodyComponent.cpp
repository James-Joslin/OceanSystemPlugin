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
// InitializeWaterBody — shared editor/runtime init
// ===================================================================

void UOceanBodyComponent::InitializeWaterBody()
{
	AActor* Owner = GetOwner();
	const FString OwnerName = Owner ? Owner->GetName() : TEXT("null");

	// --- Create MID only if it doesn't exist or parent material changed ---
	UMaterialInterface* BaseMat = BaseMaterial.LoadSynchronous();
	if (BaseMat)
	{
		if (!MaterialInstance || MaterialInstance->Parent != BaseMat)
		{
			MaterialInstance = UMaterialInstanceDynamic::Create(
				BaseMat, this,
				FName(*FString::Printf(TEXT("MID_%s"), *OwnerName)));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("OceanBodyComponent on '%s': BaseMaterial not set — no MID created."),
			*OwnerName);
	}

	// --- Register with subsystem ---
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UWaveParameterSubsystem* Subsystem = World->GetSubsystem<UWaveParameterSubsystem>();
	if (!Subsystem)
	{
		return;
	}

	// Unregister first so re-registration picks up the current MID and
	// wave config. No-op if not previously registered.
	Subsystem->UnregisterWaterBody(this);

	FWaterBodyEntry Entry = BuildRegistryEntry();
	Subsystem->RegisterWaterBody(Entry);
}

// ===================================================================
// Lifecycle
// ===================================================================

void UOceanBodyComponent::BeginPlay()
{
	Super::BeginPlay();
	InitializeWaterBody();
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
// Editor — Property Change Handling
// ===================================================================
//
// Without this, editing WaveConfig.Layers directly in the Details panel
// has no effect — the subsystem's copy of the config never updates and
// the MID never resyncs. This catches property changes and pushes the
// new values to the subsystem immediately.
//
// Note: OnConstruction also fires on every property change, but it
// only ensures MID/registration exist — it doesn't rebuild tiles.
// This handler is responsible for the lightweight "mark dirty" path.
// ===================================================================

#if WITH_EDITOR
void UOceanBodyComponent::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName MemberName = PropertyChangedEvent.GetMemberPropertyName();

	UWorld* World = GetWorld();
	UWaveParameterSubsystem* Subsystem = World
		? World->GetSubsystem<UWaveParameterSubsystem>()
		: nullptr;

	// --- WaveConfig edited directly (Layers, PhysicsLayerCount, TimeScale) ---
	if (MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, WaveConfig))
	{
		WaveConfig.SortLayers();
		WaveConfig.bDirty = true;

		if (Subsystem)
		{
			Subsystem->UpdateWaterBodyConfig(this, WaveConfig);
		}
	}

	// --- WaveGenerator property changed — auto-regenerate layers ---
	if (MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, WaveGenerator))
	{
		WaveConfig = WaveGenerator.Generate();

		if (Subsystem)
		{
			Subsystem->UpdateWaterBodyConfig(this, WaveConfig);
		}
	}

	// --- BaseMaterial changed — need new MID ---
	if (MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, BaseMaterial))
	{
		// Force MID recreation by nulling it first
		MaterialInstance = nullptr;
		InitializeWaterBody();
	}

	// --- Structural properties changed — re-register ---
	if (MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, BodyType)
		|| MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, Priority)
		|| MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, Extent))
	{
		if (Subsystem)
		{
			Subsystem->UnregisterWaterBody(this);
			Subsystem->RegisterWaterBody(BuildRegistryEntry());
		}
	}
}
#endif

// ===================================================================
// Config Updates
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

void UOceanBodyComponent::GenerateWavesFromConfig()
{
	WaveConfig = WaveGenerator.Generate();

	UE_LOG(LogTemp, Log,
		TEXT("OceanBodyComponent '%s': Generated %d wave layers."),
		GetOwner() ? *GetOwner()->GetName() : TEXT("null"),
		WaveConfig.Layers.Num());

	if (UWorld* World = GetWorld())
	{
		if (UWaveParameterSubsystem* Subsystem = World->GetSubsystem<UWaveParameterSubsystem>())
		{
			Subsystem->UpdateWaterBodyConfig(this, WaveConfig);
		}
	}
}

// ===================================================================
// Registry Entry
// ===================================================================

FWaterBodyEntry UOceanBodyComponent::BuildRegistryEntry() const
{
	FWaterBodyEntry Entry;

	Entry.Owner = const_cast<UOceanBodyComponent*>(this);
	Entry.BodyType = BodyType;
	Entry.Priority = Priority;
	Entry.WaveConfig = WaveConfig;
	Entry.WaveConfig.bDirty = true;
	Entry.MaterialInstance = MaterialInstance;

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
		Entry.RiverHalfWidth = 0.0f;
		break;
	}

	return Entry;
}
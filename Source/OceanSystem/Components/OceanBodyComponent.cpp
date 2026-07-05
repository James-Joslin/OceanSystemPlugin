// Copyright James Joslin. All Rights Reserved.

#include "OceanBodyComponent.h"
#include "TiledWaterMeshComponent.h"
#include "../Subsystem/WaveParameterSubsystem.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"

UOceanBodyComponent::UOceanBodyComponent()
{
	// Tick is available but starts disabled — only enabled when
	// bShowDebugBounds is toggled on via PostEditChangeProperty.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	// Receive OnUpdateTransform so the registry BaseZ tracks the actor
	// when it moves (editor drag or runtime SetActorLocation).
	bWantsOnUpdateTransform = true;

	WaveConfig = FWaveConfig::MakeDefaultOcean();
}

// ===================================================================
// Transform tracking — keep registry BaseZ in sync with placement
// ===================================================================
//
// Registration snapshots BaseZ once, so without this a water body
// moved after registration leaves the physics surface at the OLD
// height while the rendered surface moves — buoyancy objects then
// float in the air or sink below the visuals. Re-register whenever
// our world Z actually changes (epsilon-guarded so editor drags
// don't spam re-registration for XY-only movement).
// ===================================================================

void UOceanBodyComponent::OnUpdateTransform(
	EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);

	if (!GetWorld())
	{
		return;
	}

	const float NewZ = GetComponentLocation().Z;
	if (!FMath::IsNearlyEqual(NewZ, LastRegisteredZ, 0.5f))
	{
		InitializeWaterBody();
	}
}

// ===================================================================
// InitializeWaterBody � shared editor/runtime init
// ===================================================================

void UOceanBodyComponent::InitializeWaterBody()
{
	AActor* Owner = GetOwner();
	const FString OwnerName = Owner ? Owner->GetName() : TEXT("null");

	// --- Auto-size Extent from a sibling tiled mesh ---
	// Guarantees the subsystem's query bounds match the rendered water
	// area regardless of which actor or Blueprint hosts the components.
	if (bAutoSizeExtentFromMesh && BodyType != EOceanBodyType::River && Owner)
	{
		if (const UTiledWaterMeshComponent* Mesh =
			Owner->FindComponentByClass<UTiledWaterMeshComponent>())
		{
			const FVector2D MeshExtent(
				Mesh->TilesX * Mesh->TileSize * 0.5f,
				Mesh->TilesY * Mesh->TileSize * 0.5f);

			if (!Extent.Equals(MeshExtent, 0.1))
			{
				Extent = MeshExtent;
				UE_LOG(LogTemp, Log,
					TEXT("OceanBodyComponent on '%s': Extent auto-synced to "
						"tiled mesh (%.0f x %.0f)."),
					*OwnerName, Extent.X * 2.0f, Extent.Y * 2.0f);
			}
		}
	}

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
			TEXT("OceanBodyComponent on '%s': BaseMaterial not set � no MID created."),
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

	// Remember where we registered so OnUpdateTransform can detect
	// genuine Z changes and re-register.
	LastRegisteredZ = GetComponentLocation().Z;
}

// ===================================================================
// Lifecycle
// ===================================================================

void UOceanBodyComponent::BeginPlay()
{
	Super::BeginPlay();
	InitializeWaterBody();

	if (bShowDebugBounds)
	{
		SetComponentTickEnabled(true);
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
// Editor � Property Change Handling
// ===================================================================
//
// Without this, editing WaveConfig.Layers directly in the Details panel
// has no effect � the subsystem's copy of the config never updates and
// the MID never resyncs. This catches property changes and pushes the
// new values to the subsystem immediately.
//
// Note: OnConstruction also fires on every property change, but it
// only ensures MID/registration exist � it doesn't rebuild tiles.
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

	// --- DetailWaveConfig edited directly ---
	if (MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, DetailWaveConfig))
	{
		DetailWaveConfig.SortLayers();
		DetailWaveConfig.bDirty = true;

		if (Subsystem)
		{
			Subsystem->UpdateDetailWaveConfig(this, DetailWaveConfig);
		}
	}

	// --- WaveGenerator property changed — auto-regenerate main layers ---
	if (MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, WaveGenerator))
	{
		WaveConfig = WaveGenerator.Generate();

		if (Subsystem)
		{
			Subsystem->UpdateWaterBodyConfig(this, WaveConfig);
		}
	}

	// --- DetailWaveGenerator property changed — auto-regenerate detail layers ---
	if (MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, DetailWaveGenerator))
	{
		DetailWaveConfig = DetailWaveGenerator.Generate();

		if (Subsystem)
		{
			Subsystem->UpdateDetailWaveConfig(this, DetailWaveConfig);
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
		|| MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, Extent)
		|| MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, DomainWarpFrequency)
		|| MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, DomainWarpAmount)
		|| MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, CrestSharpness))
	{
		if (Subsystem)
		{
			Subsystem->UnregisterWaterBody(this);
			Subsystem->RegisterWaterBody(BuildRegistryEntry());
		}
	}

	// --- Debug toggle — enable/disable tick for debug drawing ---
	if (MemberName == GET_MEMBER_NAME_CHECKED(UOceanBodyComponent, bShowDebugBounds))
	{
		SetComponentTickEnabled(bShowDebugBounds);
	}
}
#endif

// ===================================================================
// Tick — Debug Bounds Visualisation
// ===================================================================
//
// Only runs when bShowDebugBounds is true (tick is disabled otherwise).
// Draws a wireframe box matching the subsystem's spatial query area so
// you can verify buoyancy actors are within the registered bounds.
// ===================================================================

void UOceanBodyComponent::TickComponent(
	float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

#if ENABLE_DRAW_DEBUG
	if (!bShowDebugBounds)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector Pos = GetComponentLocation();
	constexpr float Thickness = 3.0f;

	if (BodyType == EOceanBodyType::Ocean || BodyType == EOceanBodyType::Lake)
	{
		// Wireframe box showing the XY extent at BaseZ.
		// This is exactly the area the subsystem checks in FindWaterBodyAt.
		// Height is cosmetic — the subsystem only checks XY, not Z.
		constexpr float BoxHalfHeight = 300.0f;
		const FVector BoxCenter(Pos.X, Pos.Y, Pos.Z);
		const FVector BoxExtent(Extent.X, Extent.Y, BoxHalfHeight);

		const FColor BoundsColor = (BodyType == EOceanBodyType::Ocean)
			? FColor(30, 120, 255)   // Blue for ocean
			: FColor(50, 200, 120);  // Green for lake

		DrawDebugBox(World, BoxCenter, BoxExtent,
			BoundsColor, false, -1.0f, 0, Thickness);

		// Centre marker at BaseZ — the resting water surface height
		DrawDebugSphere(World, Pos, 40.0f, 8,
			FColor::Cyan, false, -1.0f, 0, 2.0f);

		// Label the body type and priority at origin
		const FString Label = FString::Printf(TEXT("%s  P:%d  BaseZ:%.0f"),
			BodyType == EOceanBodyType::Ocean ? TEXT("Ocean") : TEXT("Lake"),
			Priority, Pos.Z);
		DrawDebugString(World, Pos + FVector(0, 0, BoxHalfHeight + 50.0f),
			Label, nullptr, FColor::White, -1.0f, true, 1.2f);
	}
#endif
}

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

void UOceanBodyComponent::SetDetailWaveConfig(const FWaveConfig& NewConfig)
{
	DetailWaveConfig = NewConfig;
	DetailWaveConfig.SortLayers();
	DetailWaveConfig.bDirty = true;

	if (UWorld* World = GetWorld())
	{
		if (UWaveParameterSubsystem* Subsystem = World->GetSubsystem<UWaveParameterSubsystem>())
		{
			Subsystem->UpdateDetailWaveConfig(this, DetailWaveConfig);
		}
	}
}

void UOceanBodyComponent::GenerateWavesFromConfig()
{
	WaveConfig = WaveGenerator.Generate();
	DetailWaveConfig = DetailWaveGenerator.Generate();

	UE_LOG(LogTemp, Log,
		TEXT("OceanBodyComponent '%s': Generated %d main + %d detail wave layers."),
		GetOwner() ? *GetOwner()->GetName() : TEXT("null"),
		WaveConfig.Layers.Num(),
		DetailWaveConfig.Layers.Num());

	if (UWorld* World = GetWorld())
	{
		if (UWaveParameterSubsystem* Subsystem = World->GetSubsystem<UWaveParameterSubsystem>())
		{
			Subsystem->UpdateWaterBodyConfig(this, WaveConfig);
			Subsystem->UpdateDetailWaveConfig(this, DetailWaveConfig);
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
	Entry.DetailWaveConfig = DetailWaveConfig;
	Entry.DetailWaveConfig.bDirty = true;
	Entry.MaterialInstance = MaterialInstance;

	// Visual params for CPU parity
	Entry.DomainWarpFrequency = DomainWarpFrequency;
	Entry.DomainWarpAmount = DomainWarpAmount;
	Entry.CrestSharpness = CrestSharpness;

	// --- Anchor the registered surface to the RENDERED mesh ---
	// BaseZ (and the bounds centre) come from the tiled mesh's world
	// transform when one exists. Physics must float on the surface the
	// player can see; if the mesh has been offset from this component,
	// the mesh wins. The shipped water actors also hard-lock the mesh
	// to the root, so this only diverges in custom setups — warn so the
	// mismatch is visible rather than silent.
	FVector WorldPos = GetComponentLocation();

	if (const AActor* Owner = GetOwner())
	{
		if (const UTiledWaterMeshComponent* Mesh =
			Owner->FindComponentByClass<UTiledWaterMeshComponent>())
		{
			const FVector MeshPos = Mesh->GetComponentLocation();
			if (!FMath::IsNearlyEqual(MeshPos.Z, WorldPos.Z, 0.5f))
			{
				UE_LOG(LogTemp, Warning,
					TEXT("OceanBodyComponent on '%s': TiledMesh Z (%.1f) differs "
						"from body Z (%.1f). Registering BaseZ from the mesh so "
						"buoyancy matches the rendered surface. Keep the mesh at "
						"relative (0,0,0) and move the actor root instead."),
					*Owner->GetName(), MeshPos.Z, WorldPos.Z);
			}
			WorldPos = MeshPos;
		}
	}

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
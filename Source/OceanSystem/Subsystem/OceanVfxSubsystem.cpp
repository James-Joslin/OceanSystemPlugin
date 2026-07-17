// Copyright James Joslin. All Rights Reserved.

#include "OceanVfxSubsystem.h"
#include "../Types/OceanTypes.h" // STATGROUP_OceanSystem
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"

DECLARE_CYCLE_STAT(TEXT("OceanVfx Tick"), STAT_OceanVfxTick, STATGROUP_OceanSystem);

namespace OceanVfx
{
	// Niagara user parameter contract. The user redirection parameter
	// store resolves these against "User.<Name>" in the assets.
	static const FName IntensityParam(TEXT("Intensity"));
	static const FName ImpactDirectionParam(TEXT("ImpactDirection"));
	static const FName WaterVelocityParam(TEXT("WaterVelocity"));
	static const FName ExtentParam(TEXT("Extent"));

	/** Burst cooldown spatial hash cell size (cm). Two triggers within
		the same ~2 m cell share a cooldown. */
	static constexpr double CooldownCellSize = 200.0;

	/** How often the cooldown map is purged, and how old an entry must
		be to go. Keeps the map bounded on long sessions. */
	static constexpr double CooldownPurgeInterval = 30.0;
	static constexpr double CooldownPurgeAge = 30.0;
}

// ===================================================================
// Subsystem lifecycle
// ===================================================================

void UOceanVfxSubsystem::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_OceanVfxTick);
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();

	// Per-frame burst budget resets here.
	BurstsThisFrame = 0;

	// Expire sustained effects whose heartbeat lapsed. This is the whole
	// robustness story: destroyed/sunk requesters simply stop calling
	// UpdateSustained and their effects die on their own.
	for (auto It = SustainedEffects.CreateIterator(); It; ++It)
	{
		if (Now - It->Value.LastHeartbeatTime > SustainedGraceSeconds)
		{
			ReleaseEffectComponent(It->Value);
			It.RemoveCurrent();
		}
	}

	// Debug: self-heartbeat the console-spawned sustained effect.
	if (DebugSustainedSet.IsValid())
	{
		if (Now < DebugSustainedUntil)
		{
			DebugSustainedHandle = UpdateSustained(
				DebugSustainedHandle, DebugSustainedSet.Get(),
				DebugSustainedLocation, FVector::UpVector, 1.0f);
		}
		else
		{
			// Stop heartbeating; the grace timeout releases it — which
			// also exercises the exact path real requesters rely on.
			DebugSustainedSet = nullptr;
			DebugSustainedHandle.Reset();
		}
	}

	// Bounded cooldown map: purge stale entries occasionally.
	if (Now - LastCooldownPurgeTime > OceanVfx::CooldownPurgeInterval)
	{
		LastCooldownPurgeTime = Now;
		for (auto It = BurstCooldowns.CreateIterator(); It; ++It)
		{
			if (Now - It->Value > OceanVfx::CooldownPurgeAge)
			{
				It.RemoveCurrent();
			}
		}
	}
}

TStatId UOceanVfxSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOceanVfxSubsystem, STATGROUP_OceanSystem);
}

void UOceanVfxSubsystem::Deinitialize()
{
	for (auto& Pair : SustainedEffects)
	{
		ReleaseEffectComponent(Pair.Value);
	}
	SustainedEffects.Empty();
	BurstCooldowns.Empty();
	LastVariantPick.Empty();

	Super::Deinitialize();
}

// ===================================================================
// Burst API
// ===================================================================

bool UOceanVfxSubsystem::RequestBurst(
	UOceanVfxVariantSet* VariantSet,
	const FVector& Location,
	const FVector& Direction,
	float Intensity,
	const FVector& WaterVelocity,
	float Extent)
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return false; // cosmetic-only; nothing to do server-side
	}

	if (!VariantSet || VariantSet->Systems.Num() == 0)
	{
		return false;
	}

	if (BurstsThisFrame >= MaxBurstsPerFrame)
	{
		return false;
	}

	// Central distance cull.
	FVector ViewLocation;
	if (GetViewLocation(ViewLocation) &&
		FVector::Dist(ViewLocation, Location) > ResolveMaxDistance(*VariantSet))
	{
		return false;
	}

	// Per-site refire cooldown (spatial hash).
	const double Now = World->GetTimeSeconds();
	const uint64 SiteKey = MakeSpatialKey(Location);
	if (const double* LastFire = BurstCooldowns.Find(SiteKey))
	{
		if (Now - *LastFire < VariantSet->MinRefireSeconds)
		{
			return false;
		}
	}

	UNiagaraSystem* System = PickVariant(*VariantSet, /*bAvoidLastPick=*/true);
	if (!System)
	{
		return false;
	}

	UNiagaraComponent* Component =
		SpawnPooled(*System, Location, Direction, /*bAutoRelease=*/true);
	if (!Component)
	{
		return false;
	}

	PushUserParams(*Component, Direction, Intensity, WaterVelocity, Extent);

	BurstCooldowns.Add(SiteKey, Now);
	++BurstsThisFrame;
	return true;
}

// ===================================================================
// Sustained API
// ===================================================================

FOceanVfxHandle UOceanVfxSubsystem::UpdateSustained(
	FOceanVfxHandle Handle,
	UOceanVfxVariantSet* VariantSet,
	const FVector& Location,
	const FVector& Direction,
	float Intensity,
	const FVector& WaterVelocity,
	float Extent)
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer ||
		!VariantSet || VariantSet->Systems.Num() == 0)
	{
		// Invalid request: tear down anything the handle refers to.
		ReleaseSustained(Handle);
		return Handle;
	}

	// Find-or-create the logical effect entry.
	FSustainedEffect* Effect =
		Handle.IsValid() ? SustainedEffects.Find(Handle.Id) : nullptr;

	if (!Effect)
	{
		Handle.Id = NextHandleId++;
		Effect = &SustainedEffects.Add(Handle.Id);
		Effect->VariantSet = VariantSet;
	}

	Effect->LastHeartbeatTime = World->GetTimeSeconds();
	Effect->LastLocation = Location;

	// Range management with hysteresis: the logical entry persists while
	// culled, so the effect pops back in when the camera returns and the
	// requester never has to care.
	const float MaxDistance = ResolveMaxDistance(*VariantSet);
	FVector ViewLocation;
	const float Distance = GetViewLocation(ViewLocation)
		? FVector::Dist(ViewLocation, Location)
		: 0.0f;

	UNiagaraComponent* Component = Effect->Component.Get();

	if (!Component && Distance <= MaxDistance)
	{
		// (Re)activate: variant chosen once and held for the lifetime.
		UNiagaraSystem* System = Effect->ChosenSystem.Get();
		if (!System)
		{
			System = PickVariant(*VariantSet, /*bAvoidLastPick=*/false);
			Effect->ChosenSystem = System;
		}

		if (System)
		{
			Component = SpawnPooled(
				*System, Location, Direction, /*bAutoRelease=*/false);
			Effect->Component = Component;
		}
	}
	else if (Component && Distance > MaxDistance + CullHysteresis)
	{
		ReleaseEffectComponent(*Effect);
		Component = nullptr;
	}

	// The heartbeat doubles as the parameter feed.
	if (Component)
	{
		if (!Component->IsActive())
		{
			// Robustness: a finite system mistakenly authored into a
			// sustained set finishes on its own — restart it. (Fix the
			// asset; this just prevents a silent dead effect.)
			Component->Activate(/*bReset=*/true);
		}

		Component->SetWorldLocation(Location);
		if (!Direction.IsNearlyZero())
		{
			Component->SetWorldRotation(Direction.Rotation());
		}
		PushUserParams(*Component, Direction, Intensity, WaterVelocity, Extent);
	}

	return Handle;
}

void UOceanVfxSubsystem::ReleaseSustained(FOceanVfxHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	if (FSustainedEffect* Effect = SustainedEffects.Find(Handle.Id))
	{
		ReleaseEffectComponent(*Effect);
		SustainedEffects.Remove(Handle.Id);
	}

	Handle.Reset();
}

// ===================================================================
// Internal
// ===================================================================

UNiagaraSystem* UOceanVfxSubsystem::PickVariant(
	const UOceanVfxVariantSet& Set, bool bAvoidLastPick)
{
	// Collect indices of valid (non-null) systems.
	TArray<int32, TInlineAllocator<8>> ValidIndices;
	for (int32 i = 0; i < Set.Systems.Num(); ++i)
	{
		if (Set.Systems[i])
		{
			ValidIndices.Add(i);
		}
	}

	if (ValidIndices.Num() == 0)
	{
		return nullptr;
	}

	int32 Chosen = ValidIndices[FMath::RandRange(0, ValidIndices.Num() - 1)];

	if (bAvoidLastPick && ValidIndices.Num() > 1)
	{
		const int32* Last = LastVariantPick.Find(&Set);
		if (Last && Chosen == *Last)
		{
			// Shift to the next valid index — cheap, uniform enough.
			const int32 Pos = ValidIndices.IndexOfByKey(Chosen);
			Chosen = ValidIndices[(Pos + 1) % ValidIndices.Num()];
		}
	}

	LastVariantPick.Add(&Set, Chosen);
	return Set.Systems[Chosen];
}

UNiagaraComponent* UOceanVfxSubsystem::SpawnPooled(
	UNiagaraSystem& System, const FVector& Location,
	const FVector& Direction, bool bAutoRelease)
{
	// Engine-pooled spawn: AutoRelease components return to the pool when
	// the (finite) system finishes; ManualRelease components are ours to
	// ReleaseToPool(), which deactivates and reclaims once particles have
	// lived out their lifetime (the natural taper).
	return UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		GetWorld(), &System, Location,
		Direction.IsNearlyZero() ? FRotator::ZeroRotator : Direction.Rotation(),
		FVector::OneVector,
		/*bAutoDestroy=*/false,
		/*bAutoActivate=*/true,
		bAutoRelease ? ENCPoolMethod::AutoRelease : ENCPoolMethod::ManualRelease,
		/*bPreCullCheck=*/true);
}

void UOceanVfxSubsystem::PushUserParams(
	UNiagaraComponent& Component, const FVector& Direction,
	float Intensity, const FVector& WaterVelocity,
	float Extent) const
{
	Component.SetVariableFloat(
		OceanVfx::IntensityParam, FMath::Clamp(Intensity, 0.0f, 1.0f));
	Component.SetVariableVec3(
		OceanVfx::ImpactDirectionParam, Direction);
	Component.SetVariableVec3(
		OceanVfx::WaterVelocityParam, WaterVelocity);
	Component.SetVariableFloat(
		OceanVfx::ExtentParam, FMath::Max(Extent, 0.0f));
}

void UOceanVfxSubsystem::ReleaseEffectComponent(FSustainedEffect& Effect) const
{
	if (UNiagaraComponent* Component = Effect.Component.Get())
	{
		// Deactivates (stop spawning, particles taper out) and returns
		// the component to the engine pool once complete.
		Component->ReleaseToPool();
	}
	Effect.Component = nullptr;
}

bool UOceanVfxSubsystem::GetViewLocation(FVector& OutLocation) const
{
	if (const APlayerCameraManager* PCM =
		UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0))
	{
		OutLocation = PCM->GetCameraLocation();
		return true;
	}
	return false;
}

float UOceanVfxSubsystem::ResolveMaxDistance(const UOceanVfxVariantSet& Set) const
{
	return Set.MaxDistanceOverride > 0.0f
		? Set.MaxDistanceOverride
		: MaxEffectDistance;
}

uint64 UOceanVfxSubsystem::MakeSpatialKey(const FVector& Location)
{
	// 21 bits per axis, offset to keep values positive. Cell size ~2 m.
	auto Pack = [](double V) -> uint64
		{
			const int64 Q = FMath::FloorToInt64(V / OceanVfx::CooldownCellSize);
			return static_cast<uint64>(Q + (1ll << 20)) & 0x1FFFFF;
		};
	return (Pack(Location.X) << 42) | (Pack(Location.Y) << 21) | Pack(Location.Z);
}

// ===================================================================
// Debug console commands
// ===================================================================

namespace
{
	UOceanVfxVariantSet* LoadVariantSetFromArg(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Ocean.Vfx: missing variant set asset path argument."));
			return nullptr;
		}

		UOceanVfxVariantSet* Set =
			LoadObject<UOceanVfxVariantSet>(nullptr, *Args[0]);
		if (!Set)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Ocean.Vfx: could not load variant set '%s'."), *Args[0]);
		}
		return Set;
	}

	bool GetDebugSpawnPoint(UWorld* World, FVector& OutLocation)
	{
		const APlayerCameraManager* PCM =
			UGameplayStatics::GetPlayerCameraManager(World, 0);
		if (!PCM)
		{
			return false;
		}
		OutLocation = PCM->GetCameraLocation()
			+ PCM->GetCameraRotation().Vector() * 300.0f;
		return true;
	}
}

/** Friend of UOceanVfxSubsystem — the console commands need access to the
	private debug-heartbeat state and internal maps. */
class FOceanVfxDebugCommands
{
public:
	static void Burst(const TArray<FString>& Args, UWorld* World)
	{
		UOceanVfxSubsystem* Vfx =
			World ? World->GetSubsystem<UOceanVfxSubsystem>() : nullptr;
		UOceanVfxVariantSet* Set = LoadVariantSetFromArg(Args);
		FVector Location;
		if (Vfx && Set && GetDebugSpawnPoint(World, Location))
		{
			const bool bFired = Vfx->RequestBurst(
				Set, Location, FVector::UpVector, 1.0f);
			UE_LOG(LogTemp, Log, TEXT("Ocean.Vfx.Burst: %s"),
				bFired ? TEXT("fired") : TEXT("rejected (cooldown/cap/range)"));
		}
	}

	static void Sustained(const TArray<FString>& Args, UWorld* World)
	{
		UOceanVfxSubsystem* Vfx =
			World ? World->GetSubsystem<UOceanVfxSubsystem>() : nullptr;
		UOceanVfxVariantSet* Set = LoadVariantSetFromArg(Args);
		FVector Location;
		if (Vfx && Set && GetDebugSpawnPoint(World, Location))
		{
			const float Seconds =
				Args.Num() > 1 ? FCString::Atof(*Args[1]) : 5.0f;
			Vfx->DebugSustainedSet = Set;
			Vfx->DebugSustainedLocation = Location;
			Vfx->DebugSustainedUntil =
				World->GetTimeSeconds() + FMath::Max(Seconds, 0.5f);
			UE_LOG(LogTemp, Log,
				TEXT("Ocean.Vfx.Sustained: running for %.1fs"), Seconds);
		}
	}

	static void Stats(const TArray<FString>&, UWorld* World)
	{
		if (const UOceanVfxSubsystem* Vfx =
			World ? World->GetSubsystem<UOceanVfxSubsystem>() : nullptr)
		{
			int32 LiveComponents = 0;
			for (const auto& Pair : Vfx->SustainedEffects)
			{
				if (Pair.Value.Component.IsValid())
				{
					++LiveComponents;
				}
			}
			UE_LOG(LogTemp, Log,
				TEXT("OceanVfx: %d sustained entries (%d with live components), %d cooldown sites tracked"),
				Vfx->SustainedEffects.Num(), LiveComponents,
				Vfx->BurstCooldowns.Num());
		}
	}
};

static FAutoConsoleCommandWithWorldAndArgs GOceanVfxBurstCmd(
	TEXT("Ocean.Vfx.Burst"),
	TEXT("Fire a burst 3m in front of the camera. Args: <VariantSetAssetPath>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		&FOceanVfxDebugCommands::Burst));

static FAutoConsoleCommandWithWorldAndArgs GOceanVfxSustainedCmd(
	TEXT("Ocean.Vfx.Sustained"),
	TEXT("Run a sustained effect 3m in front of the camera. Args: <VariantSetAssetPath> [Seconds=5]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		&FOceanVfxDebugCommands::Sustained));

static FAutoConsoleCommandWithWorldAndArgs GOceanVfxStatsCmd(
	TEXT("Ocean.Vfx.Stats"),
	TEXT("Print live ocean VFX counts."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		&FOceanVfxDebugCommands::Stats));
// Copyright James Joslin. All Rights Reserved.

#include "OceanRockSpraySubsystem.h"
#include "OceanVfxSubsystem.h"
#include "OceanVfxSettings.h"
#include "../Types/OceanTypes.h" // STATGROUP_OceanSystem
#include "WaveParameterSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "UObject/UObjectIterator.h"

DECLARE_CYCLE_STAT(TEXT("RockSpray Tick"), STAT_RockSprayTick, STATGROUP_OceanSystem);
DECLARE_CYCLE_STAT(TEXT("RockSpray Scan"), STAT_RockSprayScan, STATGROUP_OceanSystem);

namespace OceanRockSpray
{
	/** Ocean.Vfx.Rocks.Verbose — per-crossing decision logging. */
	static bool bVerbose = false;

	/** Spatial grid cell size (cm). ~25 m — a handful of cells cover the
		active radius, each holding a modest index list. */
	static constexpr double GridCellSize = 2500.0;

	/** If a point hasn't been evaluated for this long (camera was away),
		its PrevSurfaceZ is stale — discard it rather than risk a false
		"crossing" the moment the point re-enters range. */
	static constexpr double PrevStaleSeconds = 0.5;
}

// ===================================================================
// Lifecycle
// ===================================================================

void UOceanRockSpraySubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld() || InWorld.GetNetMode() == NM_DedicatedServer)
	{
		return; // cosmetic-only
	}

	// Resolve the settings-assigned variant set once. Unset = idle.
	if (const UOceanVfxSettings* Settings = GetDefault<UOceanVfxSettings>())
	{
		VariantSet = Settings->RockImpactVariantSet.LoadSynchronous();
	}

	// Registration-driven rescans: any body arriving or leaving (streamed
	// sublevels, runtime spawns) marks the point list stale. The flag is
	// consumed on the next tick, so a burst of registrations coalesces
	// into one rescan.
	if (UWaveParameterSubsystem* Wave = InWorld.GetSubsystem<UWaveParameterSubsystem>())
	{
		Wave->OnWaterBodiesChanged.AddUObject(
			this, &UOceanRockSpraySubsystem::NotifySprayPointsDirty);
	}

	bRescanPending = true; // initial scan, first tick
}

void UOceanRockSpraySubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UWaveParameterSubsystem* Wave = World->GetSubsystem<UWaveParameterSubsystem>())
		{
			Wave->OnWaterBodiesChanged.RemoveAll(this);
		}
	}
	Super::Deinitialize();
}

bool UOceanRockSpraySubsystem::IsTickable() const
{
	// Tick while there's a pending rescan to perform or points to
	// evaluate; idle otherwise (and always idle without a variant set).
	return VariantSet != nullptr && (bRescanPending || Points.Num() > 0);
}

TStatId UOceanRockSpraySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOceanRockSpraySubsystem, STATGROUP_OceanSystem);
}

// ===================================================================
// Scan — sockets per mesh asset, composed per placed instance
// ===================================================================

void UOceanRockSpraySubsystem::RescanSprayPoints()
{
	SCOPE_CYCLE_COUNTER(STAT_RockSprayScan);

	Points.Reset();
	Grid.Reset();
	bRescanPending = false;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UWaveParameterSubsystem* Wave = World->GetSubsystem<UWaveParameterSubsystem>();
	if (!Wave)
	{
		return;
	}

	const UOceanVfxSettings* Settings = GetDefault<UOceanVfxSettings>();
	const FString Prefix = Settings->SpraySocketPrefix;
	const float Margin = Settings->VerticalScanMargin;

	// Per-mesh socket cache: composing thousands of instances should not
	// re-walk the socket array per instance.
	struct FCachedSocket
	{
		FTransform Relative;
		float IntensityMul = 1.0f;
	};
	TMap<const UStaticMesh*, TArray<FCachedSocket>> SocketCache;

	auto GetSockets = [&](const UStaticMesh* Mesh) -> const TArray<FCachedSocket>&
		{
			if (const TArray<FCachedSocket>* Found = SocketCache.Find(Mesh))
			{
				return *Found;
			}

			TArray<FCachedSocket>& Cached = SocketCache.Add(Mesh);
			for (const UStaticMeshSocket* Socket : Mesh->Sockets)
			{
				if (Socket && Socket->SocketName.ToString().StartsWith(Prefix))
				{
					FCachedSocket& Entry = Cached.AddDefaulted_GetRef();
					Entry.Relative = FTransform(
						Socket->RelativeRotation, Socket->RelativeLocation);
					// Convention: relative scale X = hero-crag multiplier.
					Entry.IntensityMul = FMath::Max(Socket->RelativeScale.X, 0.0f);
				}
			}
			return Cached;
		};

	// Diagnostic counters for the scan summary log.
	int32 NumComponentsWithSockets = 0;
	int32 NumCandidates = 0;
	int32 NumRejectedNoWater = 0;
	int32 NumRejectedMargin = 0;
	int32 NumSkippedMovable = 0;

	// Accepts a world-space candidate; pre-filters against the water.
	auto TryAddPoint = [&](const FTransform& SocketWorld, float IntensityMul)
		{
			++NumCandidates;
			const FVector Location = SocketWorld.GetLocation();

			float SurfaceZ = 0.0f;
			if (!Wave->GetFullWaveHeight(Location, SurfaceZ))
			{
				++NumRejectedNoWater; // not over any water body
				return;
			}
			if (FMath::Abs(Location.Z - SurfaceZ) > Margin)
			{
				++NumRejectedMargin; // cliff-top / seabed — never in the splash band
				return;
			}

			FSprayPoint& Point = Points.AddDefaulted_GetRef();
			Point.Location = Location;
			Point.Normal = SocketWorld.GetUnitAxis(EAxis::X);
			Point.IntensityMul = IntensityMul;
		};

	// Walk every static mesh component in this world. ISM/HISM derive
	// from UStaticMeshComponent, so one iterator catches hand-placed
	// rocks and PCG output alike.
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		UStaticMeshComponent* Component = *It;
		if (!Component || Component->GetWorld() != World ||
			!Component->IsRegistered() || !Component->GetStaticMesh())
		{
			continue;
		}

		// STATIC mobility only. The whole scan model — compose socket
		// transforms once into fixed world positions — is only valid for
		// meshes that cannot move. A movable mesh with Spray_ sockets
		// (a ship's hull) belongs to OceanVesselEffectsComponent, which
		// resolves the same sockets through the LIVE transform each tick;
		// scanning it here would freeze phantom points at spawn.
		if (Component->Mobility != EComponentMobility::Static)
		{
			if (Component->GetStaticMesh()->Sockets.Num() > 0)
			{
				++NumSkippedMovable;
			}
			continue;
		}

		const TArray<FCachedSocket>& Sockets = GetSockets(Component->GetStaticMesh());
		if (Sockets.Num() == 0)
		{
			continue;
		}
		++NumComponentsWithSockets;

		if (const UInstancedStaticMeshComponent* Ism =
			Cast<UInstancedStaticMeshComponent>(Component))
		{
			const int32 Count = Ism->GetInstanceCount();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FTransform InstanceWorld;
				if (!Ism->GetInstanceTransform(Index, InstanceWorld, /*bWorldSpace=*/true))
				{
					continue;
				}
				for (const FCachedSocket& Socket : Sockets)
				{
					TryAddPoint(Socket.Relative * InstanceWorld, Socket.IntensityMul);
				}
			}
		}
		else
		{
			const FTransform& ComponentWorld = Component->GetComponentTransform();
			for (const FCachedSocket& Socket : Sockets)
			{
				TryAddPoint(Socket.Relative * ComponentWorld, Socket.IntensityMul);
			}
		}
	}

	// Bucket survivors into the XY grid.
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		Grid.FindOrAdd(MakeCell(Points[Index].Location)).Add(Index);
	}

	UE_LOG(LogTemp, Log,
		TEXT("OceanRockSpray: scan kept %d spray points in %d grid cells "
			"(%d components had '%s' sockets, %d candidate points, "
			"%d rejected: not over water, %d rejected: outside vertical margin, "
			"%d movable components skipped)."),
		Points.Num(), Grid.Num(), NumComponentsWithSockets, *Prefix,
		NumCandidates, NumRejectedNoWater, NumRejectedMargin,
		NumSkippedMovable);
}

// ===================================================================
// Tick — crossing detection on points near the camera
// ===================================================================

void UOceanRockSpraySubsystem::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_RockSprayTick);
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (!World || DeltaTime <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	UWaveParameterSubsystem* Wave = World->GetSubsystem<UWaveParameterSubsystem>();
	UOceanVfxSubsystem* Vfx = World->GetSubsystem<UOceanVfxSubsystem>();
	if (!Wave || !Vfx || !VariantSet)
	{
		return;
	}

	// Consume a pending rescan (body registered/unregistered, PCG regen
	// notified, or the initial begin-play scan).
	if (bRescanPending)
	{
		RescanSprayPoints();
		if (Points.Num() == 0)
		{
			return;
		}
	}

	const APlayerCameraManager* PCM =
		UGameplayStatics::GetPlayerCameraManager(World, 0);
	if (!PCM)
	{
		return;
	}

	const UOceanVfxSettings* Settings = GetDefault<UOceanVfxSettings>();
	const double Now = World->GetTimeSeconds();

	TArray<int32> ActiveIndices;
	GatherPointsInRadius(
		PCM->GetCameraLocation(), Settings->ActiveRadius, ActiveIndices);

	for (const int32 Index : ActiveIndices)
	{
		FSprayPoint& Point = Points[Index];

		float SurfaceZ = 0.0f;
		if (!Wave->GetFullWaveHeight(Point.Location, SurfaceZ))
		{
			Point.bHavePrev = false;
			continue;
		}

		// Discard stale prev-frame state (camera just returned).
		if (Now - Point.LastEvalTime > OceanRockSpray::PrevStaleSeconds)
		{
			Point.bHavePrev = false;
		}

		const bool bCrossedUp =
			Point.bHavePrev &&
			Point.PrevSurfaceZ < Point.Location.Z &&
			SurfaceZ >= Point.Location.Z;

		if (OceanRockSpray::bVerbose && Point.bHavePrev && !bCrossedUp)
		{
			// Throttled "why not": one line per point per second showing
			// where the surface sits relative to the point.
			if (FMath::Fmod(Now, 1.0) < DeltaTime)
			{
				UE_LOG(LogTemp, Log,
					TEXT("RockSpray[%d]: no crossing (surface Z %.1f vs point Z %.1f, prev %.1f)"),
					Index, SurfaceZ, Point.Location.Z, Point.PrevSurfaceZ);
			}
		}

		if (bCrossedUp)
		{
			// Crossing speed from the same height signal that detected
			// the crossing — big swells detonate, ripples fizzle.
			const float CrossingSpeed =
				(SurfaceZ - Point.PrevSurfaceZ) / DeltaTime;

			if (CrossingSpeed >= Settings->MinCrossingSpeed)
			{
				// Velocity query only on candidate fires — it's the
				// expensive one (two full evaluations).
				FVector WaterVelocity = FVector::ZeroVector;
				Wave->GetWaterVelocity(Point.Location, WaterVelocity);

				// Water must be moving INTO the face (XY against the
				// outward normal). Slack epsilon: a purely vertical
				// heave (zero XY velocity) still splashes.
				const float IntoFace = FVector2D::DotProduct(
					FVector2D(WaterVelocity), FVector2D(Point.Normal));

				if (IntoFace < KINDA_SMALL_NUMBER)
				{
					const float Intensity = FMath::Clamp(
						(CrossingSpeed - Settings->MinCrossingSpeed) /
						FMath::Max(Settings->FullIntensitySpeed -
							Settings->MinCrossingSpeed, 1.0f),
						0.0f, 1.0f) * Point.IntensityMul;

					// Cooldowns, range, per-frame cap: all the VFX
					// subsystem's problem. Rejection = "not this time".
					const bool bFired = Vfx->RequestBurst(
						VariantSet, Point.Location, Point.Normal,
						Intensity, WaterVelocity);

					if (OceanRockSpray::bVerbose)
					{
						UE_LOG(LogTemp, Log,
							TEXT("RockSpray[%d]: CROSSING speed %.1f, intoFace %.1f, intensity %.2f -> %s"),
							Index, CrossingSpeed, IntoFace, Intensity,
							bFired ? TEXT("FIRED") : TEXT("rejected by VFX subsystem (range/cooldown/cap)"));
					}
				}
				else if (OceanRockSpray::bVerbose)
				{
					UE_LOG(LogTemp, Log,
						TEXT("RockSpray[%d]: crossing rejected — water moving away from face (intoFace %.1f)"),
						Index, IntoFace);
				}
			}
			else if (OceanRockSpray::bVerbose)
			{
				UE_LOG(LogTemp, Log,
					TEXT("RockSpray[%d]: crossing too slow (%.1f < min %.1f)"),
					Index, CrossingSpeed, Settings->MinCrossingSpeed);
			}
		}

		Point.PrevSurfaceZ = SurfaceZ;
		Point.LastEvalTime = Now;
		Point.bHavePrev = true;
	}
}

// ===================================================================
// Spatial grid
// ===================================================================

void UOceanRockSpraySubsystem::GatherPointsInRadius(
	const FVector& Center, float Radius, TArray<int32>& OutIndices) const
{
	const float RadiusSq = Radius * Radius;
	const FIntPoint MinCell = MakeCell(Center - FVector(Radius, Radius, 0.0));
	const FIntPoint MaxCell = MakeCell(Center + FVector(Radius, Radius, 0.0));

	// If the search rectangle spans more cell coordinates than cells that
	// actually exist, sweep the occupied cells instead — a huge radius over
	// a sparse grid must not degenerate into millions of empty map lookups.
	const int64 CellsInRect =
		static_cast<int64>(MaxCell.X - MinCell.X + 1) *
		static_cast<int64>(MaxCell.Y - MinCell.Y + 1);
	if (CellsInRect > Grid.Num())
	{
		for (const auto& Pair : Grid)
		{
			if (Pair.Key.X < MinCell.X || Pair.Key.X > MaxCell.X ||
				Pair.Key.Y < MinCell.Y || Pair.Key.Y > MaxCell.Y)
			{
				continue;
			}
			for (const int32 Index : Pair.Value)
			{
				if (FVector::DistSquared2D(Points[Index].Location, Center) <= RadiusSq)
				{
					OutIndices.Add(Index);
				}
			}
		}
		return;
	}

	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			const TArray<int32>* Cell = Grid.Find(FIntPoint(X, Y));
			if (!Cell)
			{
				continue;
			}
			for (const int32 Index : *Cell)
			{
				if (FVector::DistSquared2D(Points[Index].Location, Center) <= RadiusSq)
				{
					OutIndices.Add(Index);
				}
			}
		}
	}
}

FIntPoint UOceanRockSpraySubsystem::MakeCell(const FVector& Location)
{
	return FIntPoint(
		FMath::FloorToInt32(Location.X / OceanRockSpray::GridCellSize),
		FMath::FloorToInt32(Location.Y / OceanRockSpray::GridCellSize));
}

// ===================================================================
// Debug console commands
// ===================================================================

class FOceanRockSprayDebugCommands
{
public:
	static UOceanRockSpraySubsystem* Get(UWorld* World)
	{
		return World ? World->GetSubsystem<UOceanRockSpraySubsystem>() : nullptr;
	}

	static void Rescan(const TArray<FString>&, UWorld* World)
	{
		if (UOceanRockSpraySubsystem* Rocks = Get(World))
		{
			Rocks->RescanSprayPoints();
		}
	}

	static void Show(const TArray<FString>& Args, UWorld* World)
	{
		UOceanRockSpraySubsystem* Rocks = Get(World);
		if (!Rocks)
		{
			return;
		}
		const float Seconds =
			Args.Num() > 0 ? FMath::Max(FCString::Atof(*Args[0]), 1.0f) : 10.0f;
		for (const auto& Point : Rocks->Points)
		{
			DrawDebugDirectionalArrow(
				World, Point.Location,
				Point.Location + Point.Normal * 100.0f,
				20.0f, FColor::Cyan, false, Seconds, 0, 3.0f);
		}
		UE_LOG(LogTemp, Log, TEXT("OceanRockSpray: drawing %d points for %.0fs."),
			Rocks->Points.Num(), Seconds);
	}

	static void Stats(const TArray<FString>&, UWorld* World)
	{
		if (const UOceanRockSpraySubsystem* Rocks = Get(World))
		{
			UE_LOG(LogTemp, Log,
				TEXT("OceanRockSpray: %d points, %d grid cells, variant set %s."),
				Rocks->Points.Num(), Rocks->Grid.Num(),
				Rocks->VariantSet ? TEXT("loaded") : TEXT("UNSET (idle)"));
		}
	}
};

static FAutoConsoleCommand GOceanRocksVerboseCmd(
	TEXT("Ocean.Vfx.Rocks.Verbose"),
	TEXT("Toggle per-crossing decision logging for rock spray."),
	FConsoleCommandDelegate::CreateLambda([]()
		{
			OceanRockSpray::bVerbose = !OceanRockSpray::bVerbose;
			UE_LOG(LogTemp, Log, TEXT("OceanRockSpray: verbose %s"),
				OceanRockSpray::bVerbose ? TEXT("ON") : TEXT("OFF"));
		}));

static FAutoConsoleCommandWithWorldAndArgs GOceanRocksRescanCmd(
	TEXT("Ocean.Vfx.Rocks.Rescan"),
	TEXT("Rebuild the rock spray point list (use after runtime PCG regeneration)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		&FOceanRockSprayDebugCommands::Rescan));

static FAutoConsoleCommandWithWorldAndArgs GOceanRocksShowCmd(
	TEXT("Ocean.Vfx.Rocks.Show"),
	TEXT("Draw all discovered spray points and their normals. Args: [Seconds=10]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		&FOceanRockSprayDebugCommands::Show));

static FAutoConsoleCommandWithWorldAndArgs GOceanRocksStatsCmd(
	TEXT("Ocean.Vfx.Rocks.Stats"),
	TEXT("Print rock spray point counts and variant set state."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		&FOceanRockSprayDebugCommands::Stats));
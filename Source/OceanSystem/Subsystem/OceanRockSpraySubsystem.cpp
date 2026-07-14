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

	RescanSprayPoints();
}

bool UOceanRockSpraySubsystem::IsTickable() const
{
	// Nothing to do without points and a variant set; also keeps the
	// editor preview world quiet (scan only runs at game begin-play).
	return bScanned && Points.Num() > 0 && VariantSet != nullptr;
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
	bScanned = true;

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

	// Accepts a world-space candidate; pre-filters against the water.
	auto TryAddPoint = [&](const FTransform& SocketWorld, float IntensityMul)
		{
			const FVector Location = SocketWorld.GetLocation();

			float SurfaceZ = 0.0f;
			if (!Wave->GetFullWaveHeight(Location, SurfaceZ))
			{
				return; // not over any water body
			}
			if (FMath::Abs(Location.Z - SurfaceZ) > Margin)
			{
				return; // cliff-top / seabed socket — never in the splash band
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

		const TArray<FCachedSocket>& Sockets = GetSockets(Component->GetStaticMesh());
		if (Sockets.Num() == 0)
		{
			continue;
		}

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
		TEXT("OceanRockSpray: scan kept %d spray points in %d grid cells."),
		Points.Num(), Grid.Num());
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
					Vfx->RequestBurst(
						VariantSet, Point.Location, Point.Normal,
						Intensity, WaterVelocity);
				}
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
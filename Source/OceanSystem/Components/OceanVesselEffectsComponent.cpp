// Copyright James Joslin. All Rights Reserved.

#include "OceanVesselEffectsComponent.h"
#include "OceanBuoyancyComponent.h"
#include "../Subsystem/WaveParameterSubsystem.h"
#include "../Types/OceanTypes.h" // STATGROUP_OceanSystem
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

DECLARE_CYCLE_STAT(TEXT("VesselEffects Tick"), STAT_VesselEffectsTick, STATGROUP_OceanSystem);

// ===================================================================
// Lifecycle
// ===================================================================

UOceanVesselEffectsComponent::UOceanVesselEffectsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// Buoyancy ticks TG_PrePhysics; run after physics so we read this
	// frame's samples AND this frame's resolved velocities.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UOceanVesselEffectsComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();

	if (Owner)
	{
		Buoyancy = Owner->FindComponentByClass<UOceanBuoyancyComponent>();
		PhysicsBody = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
	}
	if (World)
	{
		WaveSubsystem = World->GetSubsystem<UWaveParameterSubsystem>();
		VfxSubsystem = World->GetSubsystem<UOceanVfxSubsystem>();
	}

	if (!Buoyancy)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("VesselEffects on '%s': no OceanBuoyancyComponent found — "
				"vessel VFX disabled (this component piggybacks its samples)."),
			Owner ? *Owner->GetName() : TEXT("null"));
		SetComponentTickEnabled(false);
		return;
	}
	if (!WaveSubsystem || !VfxSubsystem)
	{
		SetComponentTickEnabled(false);
		return;
	}

	// Read this frame's buoyancy samples, not last frame's.
	AddTickPrerequisiteComponent(Buoyancy);

	ClassifySamplePoints();
}

void UOceanVesselEffectsComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The heartbeat timeout would catch these anyway; explicit release
	// just starts the taper a few frames sooner.
	ReleaseAllHandles();
	Super::EndPlay(EndPlayReason);
}

void UOceanVesselEffectsComponent::ReleaseAllHandles()
{
	if (VfxSubsystem)
	{
		VfxSubsystem->ReleaseSustained(BowHandle);
		VfxSubsystem->ReleaseSustained(PortHandle);
		VfxSubsystem->ReleaseSustained(StarboardHandle);
		VfxSubsystem->ReleaseSustained(WakeHandle);
	}
}

// ===================================================================
// Sample classification — bow/stern/port/starboard from the layout
// ===================================================================

void UOceanVesselEffectsComponent::ClassifySamplePoints()
{
	BowIndex = SternIndex = PortIndex = StarboardIndex = INDEX_NONE;

	const TArray<FVector>& LocalPoints = Buoyancy->SamplePoints;
	if (LocalPoints.IsEmpty())
	{
		return;
	}

	float MaxX = -FLT_MAX, MinX = FLT_MAX, MinY = FLT_MAX, MaxY = -FLT_MAX;
	for (int32 Index = 0; Index < LocalPoints.Num(); ++Index)
	{
		const FVector& P = LocalPoints[Index];
		if (P.X > MaxX) { MaxX = P.X; BowIndex = Index; }
		if (P.X < MinX) { MinX = P.X; SternIndex = Index; }
		if (P.Y < MinY) { MinY = P.Y; PortIndex = Index; }
		if (P.Y > MaxY) { MaxY = P.Y; StarboardIndex = Index; }
	}

	// Degenerate layouts (e.g. a single row down the centreline): drop
	// side effects rather than double-driving one point.
	if (PortIndex == BowIndex || PortIndex == SternIndex)
	{
		PortIndex = INDEX_NONE;
	}
	if (StarboardIndex == BowIndex || StarboardIndex == SternIndex ||
		StarboardIndex == PortIndex)
	{
		StarboardIndex = INDEX_NONE;
	}
}

// ===================================================================
// Tick
// ===================================================================

void UOceanVesselEffectsComponent::TickComponent(
	float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	SCOPE_CYCLE_COUNTER(STAT_VesselEffectsTick);
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!Buoyancy || !PhysicsBody || !WaveSubsystem || !VfxSubsystem)
	{
		return;
	}

	// Stale samples = buoyancy early-outed (physics disabled, no water
	// body, ...). Do nothing; sustained effects taper via heartbeat.
	if (Buoyancy->GetSampleFrame() != GFrameCounter)
	{
		return;
	}

	const TArray<FOceanBuoyancySample>& Samples = Buoyancy->GetSampleStates();
	if (Samples.IsEmpty())
	{
		return;
	}

	const FVector BoatVelocity = PhysicsBody->GetPhysicsLinearVelocity();
	const FVector Forward =
		GetOwner()->GetActorForwardVector().GetSafeNormal2D();

	// ----- Bow spray (sustained) -----
	if (BowSpraySet && Samples.IsValidIndex(BowIndex))
	{
		const FOceanBuoyancySample& Bow = Samples[BowIndex];
		float RelForwardSpeed = 0.0f;

		if (Bow.bOverWater)
		{
			// Boat velocity minus water velocity — the whole reason the
			// velocity port exists. A boat holding station against a
			// current still sprays; one surfing with the waves doesn't.
			FVector WaterVelocity = FVector::ZeroVector;
			WaveSubsystem->GetWaterVelocity(Bow.WorldLocation, WaterVelocity);
			RelForwardSpeed =
				FVector::DotProduct(BoatVelocity - WaterVelocity, Forward);
		}

		UpdateContactSpray(BowHandle, BowSpraySet, Bow, Forward,
			RelForwardSpeed, MinSpraySpeed, 1.0f);
	}

	// ----- Side contact spray (sustained, gentler) -----
	if (SideContactSet)
	{
		// Sides reuse the bow-relative speed rule against boat speed
		// alone (per-side water velocity queries aren't worth it).
		const float ForwardSpeed = FVector::DotProduct(BoatVelocity, Forward);
		const float SideThreshold = MinSpraySpeed * SideContactSpeedFactor;

		if (Samples.IsValidIndex(PortIndex))
		{
			const FOceanBuoyancySample& S = Samples[PortIndex];
			UpdateContactSpray(PortHandle, SideContactSet, S,
				-GetOwner()->GetActorRightVector(), ForwardSpeed,
				SideThreshold, 0.7f);
		}
		if (Samples.IsValidIndex(StarboardIndex))
		{
			const FOceanBuoyancySample& S = Samples[StarboardIndex];
			UpdateContactSpray(StarboardHandle, SideContactSet, S,
				GetOwner()->GetActorRightVector(), ForwardSpeed,
				SideThreshold, 0.7f);
		}
	}

	// ----- Hull slams (burst on above->below crossings) -----
	if (SlamSet)
	{
		PrevSubmerged.SetNum(Samples.Num());
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const FOceanBuoyancySample& Sample = Samples[Index];
			const bool bWas = PrevSubmerged[Index];
			PrevSubmerged[Index] = Sample.bSubmerged;

			if (!Sample.bSubmerged || bWas)
			{
				continue; // not a fresh re-entry
			}

			const float DownSpeed = -Sample.PointVelocity.Z;
			if (DownSpeed < SlamMinImpactSpeed)
			{
				continue; // gentle settle, not a slam
			}

			const float Intensity = FMath::Clamp(
				(DownSpeed - SlamMinImpactSpeed) /
				FMath::Max(SlamFullIntensitySpeed - SlamMinImpactSpeed, 1.0f),
				0.0f, 1.0f);

			FVector WaterVelocity = FVector::ZeroVector;
			WaveSubsystem->GetWaterVelocity(Sample.WorldLocation, WaterVelocity);

			// Splash at the surface, facing up. The subsystem's per-site
			// cooldown stops a whole hull face machine-gunning at once.
			const FVector Location(
				Sample.WorldLocation.X, Sample.WorldLocation.Y, Sample.SurfaceZ);
			VfxSubsystem->RequestBurst(
				SlamSet, Location, FVector::UpVector, Intensity, WaterVelocity);
		}
	}

	// ----- Wake (sustained, along the hull line) -----
	//
	// The wake emitter is anchored at the hull MIDLINE and told the hull
	// length via User.Extent: the Niagara system spawns particles along
	// a box of that length on its local X (which points astern) with
	// small lateral velocities. Bow-spawned particles have drifted
	// furthest out by the time the boat leaves them behind, so the V
	// emerges at the bow and fans naturally — no V-shaped asset needed,
	// and one system serves any hull size.
	if (WakeSet &&
		Samples.IsValidIndex(SternIndex) && Samples.IsValidIndex(BowIndex))
	{
		const FOceanBuoyancySample& Stern = Samples[SternIndex];
		const FOceanBuoyancySample& Bow = Samples[BowIndex];
		const float Speed = BoatVelocity.Size2D();

		if (Stern.bOverWater && Speed >= WakeMinSpeed)
		{
			const float HullLength =
				FVector::Dist2D(Bow.WorldLocation, Stern.WorldLocation);

			// Spawn extent = hull + a trailing pad behind the stern;
			// anchor at the extent's midpoint so it spans bow -> pad.
			const float Extent = HullLength + WakeSternOffset;
			FVector WakeLocation =
				(Bow.WorldLocation + Stern.WorldLocation) * 0.5f
				- Forward * (WakeSternOffset * 0.5f);

			// Wake is a visual effect — pin it to the FULL visual
			// surface (detail chop included), falling back to the
			// stern's physics-LOD height off the water.
			float VisualZ = Stern.SurfaceZ;
			WaveSubsystem->GetFullWaveHeight(WakeLocation, VisualZ);
			WakeLocation.Z = VisualZ;

			const float Intensity = FMath::Clamp(
				(Speed - WakeMinSpeed) /
				FMath::Max(FullIntensitySpeed - WakeMinSpeed, 1.0f),
				0.0f, 1.0f);

			FVector WaterVelocity = FVector::ZeroVector;
			WaveSubsystem->GetWaterVelocity(WakeLocation, WaterVelocity);

			// Faces backwards: wake systems emit along local +X (astern),
			// inheriting User.WaterVelocity so foam drifts with the sea.
			WakeHandle = VfxSubsystem->UpdateSustained(
				WakeHandle, WakeSet, WakeLocation, -Forward,
				Intensity, WaterVelocity, Extent);
		}
		// else: stop heartbeating; the grace timeout tapers it.
	}
}

// ===================================================================
// Shared sustained contact-spray rule
// ===================================================================

void UOceanVesselEffectsComponent::UpdateContactSpray(
	FOceanVfxHandle& Handle, UOceanVfxVariantSet* Set,
	const FOceanBuoyancySample& Sample, const FVector& Direction,
	float RelForwardSpeed, float SpeedThreshold, float IntensityScale)
{
	// Contact: the surface sits within the waterline band of the point
	// (above or below). Fully airborne or deeply buried points don't
	// spray — spray lives where hull meets surface.
	const bool bContact =
		Sample.bOverWater && FMath::Abs(Sample.Depth) <= WaterlineBand;

	if (!bContact || RelForwardSpeed < SpeedThreshold)
	{
		return; // stop heartbeating; grace timeout tapers the effect
	}

	const float Intensity = FMath::Clamp(
		(RelForwardSpeed - SpeedThreshold) /
		FMath::Max(FullIntensitySpeed - SpeedThreshold, 1.0f),
		0.0f, 1.0f) * IntensityScale;

	// Pin to the surface so the spray rides the waterline, not the
	// (possibly briefly airborne) hull point itself.
	const FVector Location(
		Sample.WorldLocation.X, Sample.WorldLocation.Y, Sample.SurfaceZ);

	Handle = VfxSubsystem->UpdateSustained(
		Handle, Set, Location, Direction, Intensity,
		FVector::ZeroVector);
}
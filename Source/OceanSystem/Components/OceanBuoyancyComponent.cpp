// Copyright James Joslin. All Rights Reserved.

#include "OceanBuoyancyComponent.h"
#include "../Subsystem/WaveParameterSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "DrawDebugHelpers.h"

// ===================================================================
// Constructor
// ===================================================================

UOceanBuoyancyComponent::UOceanBuoyancyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// Default sample points: 4 bottom corners + centre of a 100 cm box.
	// Override via GenerateBoxSamplePoints or manual editing.
	SamplePoints.Reserve(5);
	SamplePoints.Add(FVector(-50.0f, -50.0f, -50.0f));
	SamplePoints.Add(FVector(50.0f, -50.0f, -50.0f));
	SamplePoints.Add(FVector(-50.0f, 50.0f, -50.0f));
	SamplePoints.Add(FVector(50.0f, 50.0f, -50.0f));
	SamplePoints.Add(FVector(0.0f, 0.0f, -50.0f));
}

// ===================================================================
// Lifecycle
// ===================================================================

void UOceanBuoyancyComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();

	// --- Cache subsystem ---
	if (const UWorld* World = GetWorld())
	{
		CachedSubsystem = World->GetSubsystem<UWaveParameterSubsystem>();
	}

	if (!CachedSubsystem)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("BuoyancyComponent on '%s': No WaveParameterSubsystem found. "
				"Buoyancy disabled."),
			Owner ? *Owner->GetName() : TEXT("null"));
		SetComponentTickEnabled(false);
		return;
	}

	// --- Cache physics body ---
	if (Owner)
	{
		PhysicsBody = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
	}

	if (!PhysicsBody || !PhysicsBody->IsSimulatingPhysics())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("BuoyancyComponent on '%s': Root component is not simulating physics. "
				"Enable 'Simulate Physics' on the root primitive component. "
				"Buoyancy disabled."),
			Owner ? *Owner->GetName() : TEXT("null"));
		SetComponentTickEnabled(false);
		return;
	}

	if (SamplePoints.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("BuoyancyComponent on '%s': No sample points configured. "
				"Use GenerateBoxSamplePoints or add points manually."),
			Owner ? *Owner->GetName() : TEXT("null"));
	}
}

// ===================================================================
// Tick — Buoyancy Force Application
// ===================================================================
//
// Per-point spring-damper model:
//
//   k        = Mass * g * BuoyancyMultiplier / (N * EquilibriumDepth)
//   Depth_i  = clamp(WaterZ_i - PointZ_i, 0, MaxSubmersionDepth)
//   Spring_i = k * Depth_i
//   Damper_i = -PointDamping * PointVel_i.Z * (Mass / N)
//   F_i      = (0, 0, Spring_i + Damper_i)  applied at the point
//
// Why this is stable where uniform clamped forces are not:
//
//   * Depth-proportional springs: when the body tilts, the low side is
//     deeper and pushes harder — a righting moment, exactly like real
//     displaced volume. (The old code clamped every point to identical
//     force once deep, so a deeply "submerged" body had NO righting
//     moment and equal up-forces below the CoM flipped it like an
//     inverted pendulum.)
//
//   * Per-point dampers: a rolling body moves its points vertically
//     even when its centre is stationary, so damping the POINT velocity
//     at the POINT location directly resists pitch/roll. Central
//     damping (the old DampingCoefficient) cannot produce this torque
//     no matter how high it is set.
//
//   * Mass-derived k: total buoyancy always balances weight at
//     EquilibriumDepth, so no per-object force tuning and no runaway
//     when mass or point count changes.
//
// Damper force is clamped so it can never exceed the spring force it
// opposes — the water can slow a point but never yank it downward
// harder than buoyancy pushes up, which prevents damping-induced
// instability at large timesteps.
// ===================================================================

void UOceanBuoyancyComponent::TickComponent(
	float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!PhysicsBody || !CachedSubsystem || SamplePoints.IsEmpty())
	{
		return;
	}

	// Guard: physics may have been disabled at runtime
	if (!PhysicsBody->IsSimulatingPhysics())
	{
		return;
	}

	const float Mass = PhysicsBody->GetMass();
	if (Mass <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}

	const FTransform OwnerTransform = GetOwner()->GetActorTransform();
	const int32 NumPoints = SamplePoints.Num();
	const float MassPerPoint = Mass / static_cast<float>(NumPoints);

	// Gravity magnitude from the world so custom gravity is respected.
	const float GravityZ = GetWorld()->GetGravityZ();          // typically -980
	const float Gravity = FMath::Abs(GravityZ) > UE_KINDA_SMALL_NUMBER
		? FMath::Abs(GravityZ) : 980.0f;

	// Spring constant per point (force units per cm of depth), derived
	// so that N points at EquilibriumDepth exactly balance weight.
	const float SpringPerCm =
		(Mass * Gravity * BuoyancyMultiplier)
		/ (static_cast<float>(NumPoints) * FMath::Max(EquilibriumDepth, 1.0f));

	int32 SubmergedCount = 0;

	// ----- Per-point spring-damper forces -----
	for (const FVector& LocalPoint : SamplePoints)
	{
		const FVector WorldPoint = OwnerTransform.TransformPosition(LocalPoint);

		FGerstnerResult WaveResult;
		const bool bOverWater = CachedSubsystem->GetWaveData(WorldPoint, WaveResult);

		if (!bOverWater)
		{
#if ENABLE_DRAW_DEBUG
			if (bShowDebug)
			{
				DrawDebugSphere(GetWorld(), WorldPoint, DebugPointRadius,
					8, FColor::Yellow, false, -1.0f, 0, 1.5f);
			}
#endif
			continue;
		}

		const float RawDepth = WaveResult.WorldZ - WorldPoint.Z;
		const bool bSubmerged = RawDepth > 0.0f;

		float AppliedForceZ = 0.0f;

		if (bSubmerged)
		{
			++SubmergedCount;

			const float Depth = FMath::Min(RawDepth, MaxSubmersionDepth);

			// Spring: proportional to depth — deeper points push harder,
			// producing the righting moment when the body tilts.
			const float Spring = SpringPerCm * Depth;

			// Damper: opposes THIS point's vertical velocity. Clamped to
			// the spring magnitude so water can arrest motion but never
			// apply a net downward yank stronger than the buoyancy here.
			const FVector PointVel =
				PhysicsBody->GetPhysicsLinearVelocityAtPoint(WorldPoint);
			const float DamperRaw = -PointDamping * PointVel.Z * MassPerPoint;
			const float Damper = FMath::Clamp(DamperRaw, -Spring, Spring);

			AppliedForceZ = Spring + Damper;

			PhysicsBody->AddForceAtLocation(
				FVector(0.0f, 0.0f, AppliedForceZ), WorldPoint);
		}

#if ENABLE_DRAW_DEBUG
		if (bShowDebug)
		{
			// Green = submerged, Red = above water
			const FColor PointColor = bSubmerged ? FColor::Green : FColor::Red;
			DrawDebugSphere(GetWorld(), WorldPoint, DebugPointRadius,
				8, PointColor, false, -1.0f, 0, 1.5f);

			// Cyan line from sample point to water surface
			const FVector SurfacePoint(WorldPoint.X, WorldPoint.Y, WaveResult.WorldZ);
			DrawDebugLine(GetWorld(), WorldPoint, SurfacePoint,
				FColor::Cyan, false, -1.0f, 0, 1.0f);

			// Magenta arrow showing applied force (scaled for visibility)
			if (bSubmerged)
			{
				const float ArrowLen = (AppliedForceZ / (Mass * Gravity)) * 200.0f;
				DrawDebugDirectionalArrow(GetWorld(), WorldPoint,
					WorldPoint + FVector(0, 0, ArrowLen),
					20.0f, FColor::Magenta, false, -1.0f, 0, 2.0f);
			}
		}
#endif
	}

	SubmergedFraction =
		static_cast<float>(SubmergedCount) / static_cast<float>(NumPoints);
	bIsInWater = SubmergedCount > 0;

#if ENABLE_DRAW_DEBUG
	if (bShowDebug)
	{
		const FVector DiagPoint = OwnerTransform.TransformPosition(SamplePoints[0]);
		FGerstnerResult DiagResult;
		const bool bDiagOver = CachedSubsystem->GetWaveData(DiagPoint, DiagResult);

		const FString DebugText = FString::Printf(
			TEXT("Submerged: %d/%d\n")
			TEXT("Mass: %.1f kg   Spring/cm/pt: %.1f\n")
			TEXT("PointZ: %.1f\n")
			TEXT("WaterZ: %.1f\n")
			TEXT("Depth: %.1f cm"),
			SubmergedCount, NumPoints,
			Mass, SpringPerCm,
			DiagPoint.Z,
			bDiagOver ? DiagResult.WorldZ : 0.0f,
			bDiagOver ? (DiagResult.WorldZ - DiagPoint.Z) : 0.0f);

		DrawDebugString(GetWorld(),
			OwnerTransform.GetLocation() + FVector(0, 0, 200),
			DebugText, nullptr, FColor::White, -1.0f, true, 1.0f);
	}
#endif

	// ----- Central drag (only when in water) -----
	if (!bIsInWater)
	{
		return;
	}

	const FVector LinearVelocity = PhysicsBody->GetPhysicsLinearVelocity();

	// Linear water drag — resists all movement, scaled by submersion.
	// (Vertical damping is now handled per-point above.)
	{
		const FVector Drag = -LinearVelocity * WaterDragLinear * SubmergedFraction;
		PhysicsBody->AddForce(Drag, NAME_None, /*bAccelChange=*/true);
	}

	// Angular water drag — kills residual spin (mainly yaw, which the
	// per-point vertical dampers cannot see).
	{
		const FVector AngularVelocity =
			PhysicsBody->GetPhysicsAngularVelocityInRadians();
		const FVector AngDrag =
			-AngularVelocity * WaterDragAngular * SubmergedFraction;
		PhysicsBody->AddTorqueInRadians(AngDrag, NAME_None, /*bAccelChange=*/true);
	}
}

// ===================================================================
// Editor — Auto-regenerate when box params change
// ===================================================================

#if WITH_EDITOR
void UOceanBuoyancyComponent::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropName = PropertyChangedEvent.GetMemberPropertyName();

	if (PropName == GET_MEMBER_NAME_CHECKED(UOceanBuoyancyComponent, BoxHalfExtents)
		|| PropName == GET_MEMBER_NAME_CHECKED(UOceanBuoyancyComponent, BoxPointsPerAxis))
	{
		GenerateBoxSamplePoints();
	}
}
#endif

// ===================================================================
// Sample Point Generator
// ===================================================================

void UOceanBuoyancyComponent::GenerateBoxSamplePoints()
{
#if WITH_EDITOR
	Modify();
#endif

	SamplePoints.Empty();

	const int32 N = FMath::Max(2, BoxPointsPerAxis);
	const float Divisor = static_cast<float>(N - 1);

	// Generate an N x N grid on the bottom face of the box.
	SamplePoints.Reserve(N * N);

	for (int32 jy = 0; jy < N; ++jy)
	{
		for (int32 ix = 0; ix < N; ++ix)
		{
			const float X = FMath::Lerp(-BoxHalfExtents.X, BoxHalfExtents.X,
				static_cast<float>(ix) / Divisor);
			const float Y = FMath::Lerp(-BoxHalfExtents.Y, BoxHalfExtents.Y,
				static_cast<float>(jy) / Divisor);

			SamplePoints.Add(FVector(X, Y, -BoxHalfExtents.Z));
		}
	}

	UE_LOG(LogTemp, Log,
		TEXT("BuoyancyComponent: Generated %d sample points on bottom face "
			"(%.2f x %.2f, Z=%.2f)."),
		SamplePoints.Num(),
		BoxHalfExtents.X * 2.0f, BoxHalfExtents.Y * 2.0f,
		-BoxHalfExtents.Z);
}
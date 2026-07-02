// Copyright James Joslin. All Rights Reserved.

#include "OceanBuoyancyComponent.h"
#include "../Subsystem/WaveParameterSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

// ===================================================================
// Constructor
// ===================================================================

UOceanBuoyancyComponent::UOceanBuoyancyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// Default sample points: 4 bottom corners + centre of a unit box.
	// Override via GenerateBoxSamplePoints or manual editing.
	SamplePoints.Reserve(5);
	SamplePoints.Add(FVector(-0.5f, -0.5f, -0.5f));
	SamplePoints.Add(FVector(0.5f, -0.5f, -0.5f));
	SamplePoints.Add(FVector(-0.5f, 0.5f, -0.5f));
	SamplePoints.Add(FVector(0.5f, 0.5f, -0.5f));
	SamplePoints.Add(FVector(0.0f, 0.0f, -0.5f));
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
// For each sample point:
//   1. Transform local → world
//   2. Query subsystem physics path (PhysicsLayerCount layers only)
//   3. Compute submersion depth = WaterZ − PointZ
//   4. If submerged: apply upward force at that world location
//
// After all points:
//   5. Apply vertical damping (opposes Z velocity)
//   6. Apply linear & angular drag scaled by submerged fraction
//
// Forces use AddForceAtLocation for buoyancy (creates natural torques).
// Damping/drag use bAccelChange=true (mass-independent) so the same
// coefficients work for objects of any mass.
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

	const FTransform OwnerTransform = GetOwner()->GetActorTransform();

	int32 SubmergedCount = 0;

	// ----- Per-point buoyancy forces -----
	for (const FVector& LocalPoint : SamplePoints)
	{
		const FVector WorldPoint = OwnerTransform.TransformPosition(LocalPoint);

		FGerstnerResult WaveResult;
		if (!CachedSubsystem->GetWaveData(WorldPoint, WaveResult))
		{
			continue; // Point not over any registered water body
		}

		const float Submersion = WaveResult.WorldZ - WorldPoint.Z;

		if (Submersion > 0.0f)
		{
			++SubmergedCount;

			// Upward force scaled by submersion depth.
			// SubmersionForceExponent controls the force curve:
			//   1.0 = linear, <1 = aggressive response, >1 = gentle response
			const float ForceMag = BuoyancyForce
				* FMath::Pow(Submersion, SubmersionForceExponent);

			PhysicsBody->AddForceAtLocation(
				FVector::UpVector * ForceMag,
				WorldPoint);
		}
	}

	bIsInWater = SubmergedCount > 0;

	// ----- Damping & drag (only when in water) -----
	if (!bIsInWater)
	{
		return;
	}

	const float SubmergedFraction =
		static_cast<float>(SubmergedCount) / SamplePoints.Num();

	const FVector LinearVelocity = PhysicsBody->GetPhysicsLinearVelocity();

	// Vertical damping — opposes Z velocity to reduce bobbing.
	// Applied as acceleration (mass-independent) via bAccelChange.
	{
		const FVector VerticalDamping(0.0f, 0.0f,
			-LinearVelocity.Z * DampingCoefficient);
		PhysicsBody->AddForce(VerticalDamping, NAME_None, /*bAccelChange=*/true);
	}

	// Linear water drag — resists all movement, scaled by submersion.
	{
		const FVector Drag = -LinearVelocity * WaterDragLinear * SubmergedFraction;
		PhysicsBody->AddForce(Drag, NAME_None, /*bAccelChange=*/true);
	}

	// Angular water drag — resists rotation, scaled by submersion.
	{
		const FVector AngularVelocity =
			PhysicsBody->GetPhysicsAngularVelocityInRadians();
		const FVector AngDrag =
			-AngularVelocity * WaterDragAngular * SubmergedFraction;
		PhysicsBody->AddTorqueInRadians(AngDrag, NAME_None, /*bAccelChange=*/true);
	}
}

// ===================================================================
// Sample Point Generator
// ===================================================================

void UOceanBuoyancyComponent::GenerateBoxSamplePoints()
{
	SamplePoints.Empty();

	const int32 N = FMath::Max(2, BoxPointsPerAxis);
	const float Divisor = static_cast<float>(N - 1);

	// Generate an N×N grid on the bottom face of the box.
	// Z = -BoxHalfExtents.Z (bottom), X and Y span the full extents.
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
			"(%.2f × %.2f, Z=%.2f)."),
		SamplePoints.Num(),
		BoxHalfExtents.X * 2.0f, BoxHalfExtents.Y * 2.0f,
		-BoxHalfExtents.Z);
}
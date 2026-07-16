// Copyright James Joslin. All Rights Reserved.

#include "ShipWaveExclusionComponent.h"
#include "../Subsystem/ShipWaveMaskSubsystem.h"
#include "Engine/World.h"

UShipWaveExclusionComponent::UShipWaveExclusionComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UShipWaveExclusionComponent::OnRegister()
{
	Super::OnRegister();

	if (UWorld* World = GetWorld())
	{
		if (UShipWaveMaskSubsystem* Subsystem =
			World->GetSubsystem<UShipWaveMaskSubsystem>())
		{
			Subsystem->RegisterProxy(this);
		}
	}
}

void UShipWaveExclusionComponent::OnUnregister()
{
	if (UWorld* World = GetWorld())
	{
		if (UShipWaveMaskSubsystem* Subsystem =
			World->GetSubsystem<UShipWaveMaskSubsystem>())
		{
			Subsystem->UnregisterProxy(this);
		}
	}

	Super::OnUnregister();
}

void UShipWaveExclusionComponent::GetProxyWorldData(
	FVector2D& OutCenter,
	FVector2D& OutForward,
	FVector2D& OutRight,
	FVector4& OutShapeParams0,
	FVector4& OutShapeParams1) const
{
	const FTransform Transform = GetComponentTransform();
	const FVector Location = Transform.GetLocation();

	const FQuat YawOffset(
		FVector::UpVector,
		FMath::DegreesToRadians(LocalYawOffsetDegrees));

	const FQuat EffectiveRotation = Transform.GetRotation() * YawOffset;
	const FVector Forward3 = EffectiveRotation.GetForwardVector().GetSafeNormal2D();
	const FVector Right3 = EffectiveRotation.GetRightVector().GetSafeNormal2D();

	const FVector Scale = Transform.GetScale3D().GetAbs();
	const float ScaleX = FMath::Max(Scale.X, UE_KINDA_SMALL_NUMBER);
	const float ScaleY = FMath::Max(Scale.Y, UE_KINDA_SMALL_NUMBER);
	const float RadiusScale = FMath::Min(ScaleX, ScaleY);

	OutCenter = FVector2D(Location.X, Location.Y);
	OutForward = FVector2D(Forward3.X, Forward3.Y);
	OutRight = FVector2D(Right3.X, Right3.Y);

	// X = capsule half length
	// Y = capsule radius
	// Z = rounded-box half extent X
	// W = rounded-box half extent Y
	OutShapeParams0 = FVector4(
		CapsuleHalfLength * ScaleX,
		CapsuleRadius * RadiusScale,
		RoundedBoxHalfExtents.X * ScaleX,
		RoundedBoxHalfExtents.Y * ScaleY);

	// X = corner radius
	// Y = fade depth
	// Z = shape type (0 box, 1 capsule)
	// W = enabled
	OutShapeParams1 = FVector4(
		CornerRadius * RadiusScale,
		InteriorFadeDepth,
		Shape == EShipWaveProxyShape::Capsule ? 1.0f : 0.0f,
		bEnabled ? 1.0f : 0.0f);
}

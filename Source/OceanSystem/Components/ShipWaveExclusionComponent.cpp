// ShipWaveExclusionComponent.cpp

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
        if (UShipWaveMaskSubsystem* Subsystem = World->GetSubsystem<UShipWaveMaskSubsystem>())
        {
            Subsystem->RegisterProxy(this);
        }
    }
}

void UShipWaveExclusionComponent::OnUnregister()
{
    if (UWorld* World = GetWorld())
    {
        if (UShipWaveMaskSubsystem* Subsystem = World->GetSubsystem<UShipWaveMaskSubsystem>())
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

    const FQuat YawOffset(FVector::UpVector, FMath::DegreesToRadians(LocalYawOffsetDegrees));
    const FVector Forward3 = (Transform.GetRotation() * YawOffset).GetForwardVector().GetSafeNormal2D();
    const FVector Right3 = (Transform.GetRotation() * YawOffset).GetRightVector().GetSafeNormal2D();

    // Use world-scaled dimensions but keep the analytical SDF solid.
    const FVector Scale = Transform.GetScale3D().GetAbs();
    const float ScaleX = FMath::Max(Scale.X, UE_KINDA_SMALL_NUMBER);
    const float ScaleY = FMath::Max(Scale.Y, UE_KINDA_SMALL_NUMBER);
    const float UniformRadiusScale = FMath::Min(ScaleX, ScaleY);

    OutCenter = FVector2D(Location.X, Location.Y);
    OutForward = FVector2D(Forward3.X, Forward3.Y);
    OutRight = FVector2D(Right3.X, Right3.Y);

    // x=half length, y=radius, z=box half X, w=box half Y
    OutShapeParams0 = FVector4(
        CapsuleHalfLength * ScaleX,
        CapsuleRadius * UniformRadiusScale,
        RoundedBoxHalfExtents.X * ScaleX,
        RoundedBoxHalfExtents.Y * ScaleY);

    // x=corner radius, y=fade depth, z=shape type, w=enabled
    OutShapeParams1 = FVector4(
        CornerRadius * UniformRadiusScale,
        InteriorFadeDepth,
        Shape == EShipWaveProxyShape::Capsule ? 1.0f : 0.0f,
        bEnabled ? 1.0f : 0.0f);
}

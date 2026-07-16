// ShipWaveExclusionComponent.h
// Solid 2D proxy used to attenuate visual Gerstner amplitude under a vessel.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "ShipWaveExclusionComponent.generated.h"

UENUM(BlueprintType)
enum class EShipWaveProxyShape : uint8
{
    RoundedBox UMETA(DisplayName = "Rounded Box"),
    Capsule    UMETA(DisplayName = "Capsule")
};

UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UShipWaveExclusionComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UShipWaveExclusionComponent();

    /** Enables this ship's contribution to the shared mask. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion")
    bool bEnabled = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion")
    EShipWaveProxyShape Shape = EShipWaveProxyShape::RoundedBox;

    /**
     * Rounded-box half extents in this component's local XY plane, in cm.
     * X is vessel forward, Y is vessel right.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion|Rounded Box",
        meta = (ClampMin = "1.0"))
    FVector2D RoundedBoxHalfExtents = FVector2D(500.0f, 150.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion|Rounded Box",
        meta = (ClampMin = "0.0"))
    float CornerRadius = 100.0f;

    /** Capsule centre-line half length in cm. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion|Capsule",
        meta = (ClampMin = "0.0"))
    float CapsuleHalfLength = 350.0f;

    /** Capsule radius in cm. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion|Capsule",
        meta = (ClampMin = "1.0"))
    float CapsuleRadius = 150.0f;

    /**
     * Width of the wave-amplitude transition INSIDE the proxy boundary.
     * At the proxy boundary WaveFade is 1; farther inside it approaches 0.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion",
        meta = (ClampMin = "1.0"))
    float InteriorFadeDepth = 250.0f;

    /** Extra local-space rotation around Z, useful when the mesh forward axis differs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion")
    float LocalYawOffsetDegrees = 0.0f;

    /** Return world-space data consumed by the mask subsystem. */
    void GetProxyWorldData(
        FVector2D& OutCenter,
        FVector2D& OutForward,
        FVector2D& OutRight,
        FVector4& OutShapeParams0,
        FVector4& OutShapeParams1) const;

protected:
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
};

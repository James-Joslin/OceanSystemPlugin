// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "ShipWaveExclusionComponent.generated.h"

class UOceanBodyComponent;

UENUM(BlueprintType)
enum class EShipWaveProxyShape : uint8
{
	RoundedBox UMETA(DisplayName = "Rounded Box"),
	Capsule    UMETA(DisplayName = "Capsule")
};

/**
 * Solid analytical XY proxy used to attenuate VISUAL Gerstner amplitude
 * beneath a vessel. It does not sample the rendered ship mesh, so hollow
 * meshes, Nanite, LODs, and non-uniform visual geometry are irrelevant.
 *
 * The component may target a specific tiled water body, or automatically
 * select the closest registered non-river water surface containing it.
 */
UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UShipWaveExclusionComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UShipWaveExclusionComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion")
	bool bEnabled = true;

	/**
	 * Optional explicit target. Leave null to automatically select the
	 * closest registered Ocean/Lake body whose XY bounds contain the ship.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion")
	TObjectPtr<UOceanBodyComponent> TargetWaterBody = nullptr;

	/**
	 * Maximum vertical distance from the ship component to a candidate
	 * water body's base surface when automatic selection is used.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion",
		meta = (ClampMin = "0.0", UIMin = "100.0", UIMax = "10000.0"))
	float MaxAutoAssignVerticalDistance = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion")
	EShipWaveProxyShape Shape = EShipWaveProxyShape::RoundedBox;

	/**
	 * Rounded-box half extents in this component's local XY plane, in cm.
	 * Local X is vessel forward and local Y is vessel right.
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
	 * Width of the transition INSIDE the proxy boundary.
	 * Boundary/outside = full waves. Deeper inside = reduced waves.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion",
		meta = (ClampMin = "1.0"))
	float InteriorFadeDepth = 250.0f;

	/** Extra local-space yaw if the imported mesh does not use +X forward. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Exclusion")
	float LocalYawOffsetDegrees = 0.0f;

	/** Returns the body explicitly selected by the user, if any. */
	UOceanBodyComponent* GetExplicitTargetWaterBody() const { return TargetWaterBody; }

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

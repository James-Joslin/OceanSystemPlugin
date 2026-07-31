// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "../Types/OceanTypes.h"
#include "WaterBodyJunctionComponent.generated.h"

class UMaterialInstanceDynamic;
class UOceanBodyComponent;
class USplineComponent;
class UTiledWaterMeshComponent;

/**
 * Geometry-owned transition between one open river endpoint and a flat
 * lake/ocean body. The component owns the only rendered surface inside its
 * footprint and its MID evaluates both wave configurations.
 */
UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UWaterBodyJunctionComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	UWaterBodyJunctionComponent(const FObjectInitializer& ObjectInitializer);

	/** Configure and rebuild from explicit river connection data. */
	bool ConfigureJunction(
		UOceanBodyComponent* InSourceBody,
		USplineComponent* InRiverSpline,
		float InRiverWidth,
		const FWaterBodyConnectionConfig& InConfig);

	/** Regenerate geometry, target cutout, MID, and subsystem bindings. */
	UFUNCTION(BlueprintCallable, Category = "Water Junction")
	bool RebuildJunction();

	/** Remove geometry, target cutout, and subsystem bindings. */
	UFUNCTION(BlueprintCallable, Category = "Water Junction")
	void ClearJunction();

	UFUNCTION(BlueprintPure, Category = "Water Junction")
	bool IsJunctionValid() const { return bJunctionValid; }

	UMaterialInstanceDynamic* GetJunctionMID() const { return JunctionMID; }
	const TArray<FVector>& GetFootprintWorld() const { return FootprintWorld; }

protected:
	virtual void OnUnregister() override;

private:
	bool BuildGeometry();
	bool RegisterTargetCutout();
	void UnregisterTargetCutout();
	void RegisterSubsystemBindings();
	void UpdateMIDGeometryParameters(
		const FVector& WorldStart,
		const FVector& WorldDirection,
		const FVector& WorldRight);

	UPROPERTY(Transient)
	TObjectPtr<UOceanBodyComponent> SourceBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UOceanBodyComponent> TargetBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USplineComponent> RiverSpline = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> JunctionMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTiledWaterMeshComponent> TargetTiledMesh = nullptr;

	FWaterBodyConnectionConfig Config;
	float RiverWidth = 500.0f;
	TArray<FVector> FootprintWorld;
	bool bJunctionValid = false;
};

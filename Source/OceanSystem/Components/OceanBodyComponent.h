// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "../Types/OceanTypes.h"
#include "WaveGenerator.h"
#include "OceanBodyComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UWaveParameterSubsystem;

UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UOceanBodyComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UOceanBodyComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body")
	EOceanBodyType BodyType = EOceanBodyType::Ocean;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Waves")
	FWaveConfig WaveConfig;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Bounds",
		meta = (EditCondition = "BodyType != EOceanBodyType::River"))
	FVector2D Extent = FVector2D(10000.0, 10000.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body")
	int32 Priority = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Material")
	TSoftObjectPtr<UMaterialInterface> BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Blending",
		meta = (ClampMin = "0.0", UIMin = "50.0", UIMax = "1000.0"))
	float BlendWidth = 200.0f;

	// -------------------------------------------------------------------
	// Wave Generator
	// -------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Wave Generator")
	FWaveGeneratorConfig WaveGenerator;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Water Body|Wave Generator")
	void GenerateWavesFromConfig();

	// -------------------------------------------------------------------
	// Initialisation
	// -------------------------------------------------------------------

	/**
	 * Create MID (if needed) and register with the wave subsystem.
	 * Called from the owning actor's OnConstruction (editor preview)
	 * and from BeginPlay (runtime). Safe to call multiple times —
	 * skips MID recreation if the parent material hasn't changed.
	 */
	void InitializeWaterBody();

	// -------------------------------------------------------------------
	// Runtime
	// -------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "Water Body")
	UMaterialInstanceDynamic* GetMaterialInstance() const { return MaterialInstance; }

	UFUNCTION(BlueprintCallable, Category = "Water Body|Waves")
	void SetWaveConfig(const FWaveConfig& NewConfig);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FWaterBodyEntry BuildRegistryEntry() const;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> MaterialInstance = nullptr;
};
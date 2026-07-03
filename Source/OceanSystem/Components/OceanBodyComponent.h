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

	/** Short-wavelength detail layers — evaluated per-pixel for normals only.
		Never displace vertices. Never evaluated on CPU. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Waves|Detail")
	FWaveConfig DetailWaveConfig;

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
	// Visual Wave Shaping (GPU + CPU parity)
	// -------------------------------------------------------------------

	/** Bends straight Gerstner crests into organic curves.
		Lower = broader bends, higher = tighter curvature. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Visual",
		meta = (ClampMin = "0.0", UIMin = "0.0001", UIMax = "0.001"))
	float DomainWarpFrequency = 0.00035f;

	/** How far the domain warp displaces positions in world units.
		0 = no warp (straight crests). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Visual",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "1000.0"))
	float DomainWarpAmount = 400.0f;

	/** Asymmetric crest/trough shaping. 1.0 = standard sine (symmetric).
		1.5+ = peaked crests with broad flat troughs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Visual",
		meta = (ClampMin = "0.5", ClampMax = "4.0", UIMin = "1.0", UIMax = "3.0"))
	float CrestSharpness = 1.5f;

	// -------------------------------------------------------------------
	// Wave Generator
	// -------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Wave Generator")
	FWaveGeneratorConfig WaveGenerator;

	/** Generator for per-pixel detail normal layers.
		Short wavelengths, higher steepness — never displace geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Body|Wave Generator|Detail")
	FWaveGeneratorConfig DetailWaveGenerator;

	/** Regenerate both main and detail wave configs from their generators. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Water Body|Wave Generator")
	void GenerateWavesFromConfig();

	// -------------------------------------------------------------------
	// Initialisation
	// -------------------------------------------------------------------

	/**
	 * Create MID (if needed) and register with the wave subsystem.
	 * Called from the owning actor's OnConstruction (editor preview)
	 * and from BeginPlay (runtime). Safe to call multiple times �
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

	/** Update the detail wave config at runtime. Pushes to the subsystem MID. */
	UFUNCTION(BlueprintCallable, Category = "Water Body|Waves")
	void SetDetailWaveConfig(const FWaveConfig& NewConfig);

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
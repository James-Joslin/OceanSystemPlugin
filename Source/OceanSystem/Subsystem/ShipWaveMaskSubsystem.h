// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ShipWaveMaskSubsystem.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UOceanBodyComponent;
class UShipWaveExclusionComponent;
class UTextureRenderTarget2D;

USTRUCT()
struct FShipWaveBodyMaskContext
{
	GENERATED_BODY()

public:

	UPROPERTY(Transient)
	TObjectPtr<UOceanBodyComponent> WaterBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> OceanMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> MaskRenderTarget = nullptr;

	FVector2D WorldOrigin = FVector2D::ZeroVector;
	FVector2D WorldSize = FVector2D(100.0f, 100.0f);
	float BaseZ = 0.0f;
	int32 Resolution = 512;
};

/**
 * Maintains one shared ship-wave mask render target PER tiled water body.
 *
 * This prevents ships on vertically stacked/overlapping lakes or oceans
 * from affecting each other. Every tile and LOD section belonging to one
 * body samples that body's single render target through its shared MID.
 *
 * River bodies are deliberately ignored.
 */
UCLASS()
class OCEANSYSTEM_API UShipWaveMaskSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UShipWaveMaskSubsystem();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return false; }

	/**
	 * Configure the common proxy stamp material. Call once at runtime.
	 * Water bodies may register before or after this call.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ocean|Ship Wave Mask")
	void Configure(
		UMaterialInterface* StampMaterial,
		int32 DefaultResolution = 512);

	UFUNCTION(BlueprintPure, Category = "Ocean|Ship Wave Mask")
	bool IsConfigured() const { return StampMID != nullptr; }

	/**
	 * Register one non-river water body and its unique MID.
	 * The subsystem creates one RT covering that body's Extent.
	 */
	void RegisterWaterBody(
		UOceanBodyComponent* WaterBody,
		UMaterialInstanceDynamic* OceanMID,
		int32 Resolution = 0);

	void UnregisterWaterBody(UOceanBodyComponent* WaterBody);

	void RegisterProxy(UShipWaveExclusionComponent* Proxy);
	void UnregisterProxy(UShipWaveExclusionComponent* Proxy);

private:
	void EnsureContextResources(FShipWaveBodyMaskContext& Context);
	void UpdateContextMapping(FShipWaveBodyMaskContext& Context);
	void RebuildBodyMask(FShipWaveBodyMaskContext& Context);
	void PushContextToMID(FShipWaveBodyMaskContext& Context);

	UOceanBodyComponent* ResolveWaterBodyForProxy(
		const UShipWaveExclusionComponent* Proxy) const;

	bool IsProxyInsideBodyXY(
		const UShipWaveExclusionComponent* Proxy,
		const UOceanBodyComponent* Body) const;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> StampMID = nullptr;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UOceanBodyComponent>, FShipWaveBodyMaskContext> BodyContexts;

	TSet<TWeakObjectPtr<UShipWaveExclusionComponent>> Proxies;

	int32 DefaultMaskResolution = 512;
};

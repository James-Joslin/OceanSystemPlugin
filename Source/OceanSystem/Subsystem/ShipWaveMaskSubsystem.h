// ShipWaveMaskSubsystem.h
// Shared world-space render target containing all active ship proxy gradients.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ShipWaveMaskSubsystem.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UShipWaveExclusionComponent;
class UTextureRenderTarget2D;

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
     * Must be called once after BeginPlay, normally by the ocean actor.
     * StampMaterial is the additive full-screen proxy-stamp material.
     */
    UFUNCTION(BlueprintCallable, Category = "Ocean|Ship Wave Mask")
    void Configure(
        UMaterialInterface* StampMaterial,
        int32 Resolution = 512,
        FVector2D WorldSize = FVector2D(100000.0f, 100000.0f));

    /** Optional explicit centre. When automatic centring is enabled this is overwritten. */
    UFUNCTION(BlueprintCallable, Category = "Ocean|Ship Wave Mask")
    void SetMaskCenter(FVector2D NewCenter);

    UFUNCTION(BlueprintCallable, Category = "Ocean|Ship Wave Mask")
    void SetFollowPlayerCamera(bool bEnable) { bFollowPlayerCamera = bEnable; }

    /** Push texture and mapping values into the ocean MID. Call once; subsystem refreshes it. */
    UFUNCTION(BlueprintCallable, Category = "Ocean|Ship Wave Mask")
    void BindOceanMID(UMaterialInstanceDynamic* OceanMID);

    UFUNCTION(BlueprintPure, Category = "Ocean|Ship Wave Mask")
    UTextureRenderTarget2D* GetMaskRenderTarget() const { return MaskRenderTarget; }

    void RegisterProxy(UShipWaveExclusionComponent* Proxy);
    void UnregisterProxy(UShipWaveExclusionComponent* Proxy);

private:
    void RebuildMask();
    void RefreshBoundMIDs();

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> MaskRenderTarget = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> StampMID = nullptr;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UMaterialInstanceDynamic>> BoundOceanMIDs;

    TSet<TWeakObjectPtr<UShipWaveExclusionComponent>> Proxies;

    FVector2D MaskCenter = FVector2D::ZeroVector;
    FVector2D MaskWorldSize = FVector2D(100000.0f, 100000.0f);
    int32 MaskResolution = 512;

    bool bConfigured = false;
    bool bFollowPlayerCamera = true;
};

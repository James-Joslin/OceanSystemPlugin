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

/** Shared rolling fields for one connected river/junction/flat-water network. */
USTRUCT()
struct FShipWaveNetworkContext
{
	GENERATED_BODY()

	FGuid NetworkId;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UOceanBodyComponent>> WaterBodies;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SurfaceMIDs;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> SuppressionRenderTarget = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DeformationRenderTarget = nullptr;

	FVector2D WorldOrigin = FVector2D::ZeroVector;
	FVector2D WorldSize = FVector2D(20000.0f, 20000.0f);
	int32 Resolution = 2048;
};

/** One active geometry junction. Connections are topology edges, not fields. */
USTRUCT()
struct FShipWaveConnectionContext
{
	GENERATED_BODY()

	FGuid ConnectionId;

	UPROPERTY(Transient)
	TObjectPtr<UOceanBodyComponent> SourceBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UOceanBodyComponent> TargetBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> JunctionMID = nullptr;
};

/**
 * Maintains shared rolling suppression and deformation fields per connected
 * water-surface network.
 *
 * A standalone body is a valid one-body network. Geometry junctions merge
 * those networks, so tiled water, a junction, and its river all sample the
 * same world-space fields without making deformation depend on a junction.
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

	/** Configure the signed hull-deformation stamp material and rolling field. */
	UFUNCTION(BlueprintCallable, Category = "Ocean|Ship Wave Mask")
	void ConfigureDeformationField(
		UMaterialInterface* DeformationStampMaterial,
		int32 FieldResolution = 2048,
		float FieldWorldSize = 20000.0f);

	UFUNCTION(BlueprintPure, Category = "Ocean|Ship Wave Mask")
	bool IsConfigured() const
	{
		return StampMID != nullptr || DeformationStampMID != nullptr;
	}

	/**
	 * Register one water surface and its unique MID. This immediately creates
	 * a standalone network; rivers are supported as well as tiled bodies.
	 */
	void RegisterWaterBody(
		UOceanBodyComponent* WaterBody,
		UMaterialInstanceDynamic* OceanMID,
		int32 Resolution = 0);

	void UnregisterWaterBody(UOceanBodyComponent* WaterBody);

	/** Add/update a junction edge. Connected components share one field pair. */
	void RegisterWaterConnection(
		const FGuid& ConnectionId,
		UOceanBodyComponent* SourceBody,
		UOceanBodyComponent* TargetBody,
		UMaterialInstanceDynamic* JunctionMID);

	/** Remove a junction edge and split its field network if necessary. */
	void UnregisterWaterConnection(const FGuid& ConnectionId);

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

	void EnsureNetworkResources(FShipWaveNetworkContext& Context);
	void UpdateNetworkMapping(FShipWaveNetworkContext& Context);
	void RebuildNetworkFields(FShipWaveNetworkContext& Context);
	void PushNetworkToMIDs(FShipWaveNetworkContext& Context);
	void RebuildNetworkTopology();
	bool DoesProxyAffectNetwork(
		const UShipWaveExclusionComponent* Proxy,
		const FShipWaveNetworkContext& Context) const;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> StampMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DeformationStampMID = nullptr;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UOceanBodyComponent>, FShipWaveBodyMaskContext> BodyContexts;

	UPROPERTY(Transient)
	TMap<FGuid, FShipWaveNetworkContext> NetworkContexts;

	/** Every rendered body is a one-node network before junctions are added. */
	UPROPERTY(Transient)
	TMap<TObjectPtr<UOceanBodyComponent>, TObjectPtr<UMaterialInstanceDynamic>>
		RegisteredSurfaces;

	UPROPERTY(Transient)
	TMap<FGuid, FShipWaveConnectionContext> WaterConnections;

	/** Authoritative lookup used to prevent legacy body fields overriding networks. */
	UPROPERTY(Transient)
	TMap<TObjectPtr<UOceanBodyComponent>, FGuid> BodyToNetwork;

	TSet<TWeakObjectPtr<UShipWaveExclusionComponent>> Proxies;

	int32 DefaultMaskResolution = 512;
	int32 DefaultFieldResolution = 2048;
	float DefaultFieldWorldSize = 20000.0f;
};

// Copyright James Joslin. All Rights Reserved.

#include "ShipWaveMaskSubsystem.h"
#include "../Components/ShipWaveExclusionComponent.h"
#include "../Components/OceanBodyComponent.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

UShipWaveMaskSubsystem::UShipWaveMaskSubsystem()
{
}

void UShipWaveMaskSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UShipWaveMaskSubsystem::Deinitialize()
{
	Proxies.Empty();
	BodyContexts.Empty();
	StampMID = nullptr;

	Super::Deinitialize();
}

TStatId UShipWaveMaskSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UShipWaveMaskSubsystem,
		STATGROUP_Tickables);
}

void UShipWaveMaskSubsystem::Configure(
	UMaterialInterface* StampMaterial,
	int32 DefaultResolution)
{
	if (!StampMaterial)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ShipWaveMaskSubsystem: Configure requires a valid stamp material."));
		return;
	}

	DefaultMaskResolution = FMath::Clamp(DefaultResolution, 64, 4096);
	StampMID = UMaterialInstanceDynamic::Create(StampMaterial, this);

	for (TPair<TObjectPtr<UOceanBodyComponent>, FShipWaveBodyMaskContext>& Pair
		: BodyContexts)
	{
		EnsureContextResources(Pair.Value);
		UpdateContextMapping(Pair.Value);
		PushContextToMID(Pair.Value);
	}
}

void UShipWaveMaskSubsystem::RegisterWaterBody(
	UOceanBodyComponent* WaterBody,
	UMaterialInstanceDynamic* OceanMID,
	int32 Resolution)
{
	if (!IsValid(WaterBody) || !IsValid(OceanMID))
	{
		return;
	}

	// Rivers use spline meshes and are intentionally outside this system.
	if (WaterBody->BodyType == EOceanBodyType::River)
	{
		UnregisterWaterBody(WaterBody);
		return;
	}

	FShipWaveBodyMaskContext& Context =
		BodyContexts.FindOrAdd(WaterBody);

	Context.WaterBody = WaterBody;
	Context.OceanMID = OceanMID;
	Context.Resolution = Resolution > 0
		? FMath::Clamp(Resolution, 64, 4096)
		: DefaultMaskResolution;

	UpdateContextMapping(Context);
	EnsureContextResources(Context);
	PushContextToMID(Context);
}

void UShipWaveMaskSubsystem::UnregisterWaterBody(
	UOceanBodyComponent* WaterBody)
{
	if (!WaterBody)
	{
		return;
	}

	if (FShipWaveBodyMaskContext* Context = BodyContexts.Find(WaterBody))
	{
		// Leave the MID in a safe full-wave state if it still exists.
		if (IsValid(Context->OceanMID))
		{
			Context->OceanMID->SetScalarParameterValue(
				TEXT("ShipWaveMaskEnabled"), 0.0f);
		}
	}

	BodyContexts.Remove(WaterBody);
}

void UShipWaveMaskSubsystem::RegisterProxy(
	UShipWaveExclusionComponent* Proxy)
{
	if (Proxy)
	{
		Proxies.Add(Proxy);
	}
}

void UShipWaveMaskSubsystem::UnregisterProxy(
	UShipWaveExclusionComponent* Proxy)
{
	if (Proxy)
	{
		Proxies.Remove(Proxy);
	}
}

void UShipWaveMaskSubsystem::Tick(float DeltaTime)
{
	if (!StampMID)
	{
		return;
	}

	for (auto It = BodyContexts.CreateIterator(); It; ++It)
	{
		FShipWaveBodyMaskContext& Context = It.Value();

		if (!IsValid(Context.WaterBody) || !IsValid(Context.OceanMID))
		{
			It.RemoveCurrent();
			continue;
		}

		UpdateContextMapping(Context);
		EnsureContextResources(Context);
		RebuildBodyMask(Context);
		PushContextToMID(Context);
	}
}

void UShipWaveMaskSubsystem::EnsureContextResources(
	FShipWaveBodyMaskContext& Context)
{
	if (!StampMID || !IsValid(Context.WaterBody))
	{
		return;
	}

	const bool bNeedsNewTarget =
		!IsValid(Context.MaskRenderTarget)
		|| Context.MaskRenderTarget->SizeX != Context.Resolution
		|| Context.MaskRenderTarget->SizeY != Context.Resolution;

	if (!bNeedsNewTarget)
	{
		return;
	}

	Context.MaskRenderTarget =
		NewObject<UTextureRenderTarget2D>(this);

	Context.MaskRenderTarget->RenderTargetFormat = RTF_R8;
	Context.MaskRenderTarget->Filter = TF_Bilinear;
	Context.MaskRenderTarget->AddressX = TA_Clamp;
	Context.MaskRenderTarget->AddressY = TA_Clamp;
	Context.MaskRenderTarget->ClearColor = FLinearColor::Black;
	Context.MaskRenderTarget->InitAutoFormat(
		Context.Resolution,
		Context.Resolution);
	Context.MaskRenderTarget->UpdateResourceImmediate(true);
}

void UShipWaveMaskSubsystem::UpdateContextMapping(
	FShipWaveBodyMaskContext& Context)
{
	if (!IsValid(Context.WaterBody))
	{
		return;
	}

	const FVector BodyLocation =
		Context.WaterBody->GetComponentLocation();

	Context.WorldSize = FVector2D(
		FMath::Max(Context.WaterBody->Extent.X * 2.0f, 100.0f),
		FMath::Max(Context.WaterBody->Extent.Y * 2.0f, 100.0f));

	const FVector2D Center(
		BodyLocation.X,
		BodyLocation.Y);

	Context.WorldOrigin =
		Center - Context.WorldSize * 0.5f;

	Context.BaseZ = BodyLocation.Z;
}

bool UShipWaveMaskSubsystem::IsProxyInsideBodyXY(
	const UShipWaveExclusionComponent* Proxy,
	const UOceanBodyComponent* Body) const
{
	if (!IsValid(Proxy) || !IsValid(Body))
	{
		return false;
	}

	const FVector ProxyLocation = Proxy->GetComponentLocation();
	const FVector BodyLocation = Body->GetComponentLocation();

	return
		FMath::Abs(ProxyLocation.X - BodyLocation.X) <= Body->Extent.X
		&& FMath::Abs(ProxyLocation.Y - BodyLocation.Y) <= Body->Extent.Y;
}

UOceanBodyComponent* UShipWaveMaskSubsystem::ResolveWaterBodyForProxy(
	const UShipWaveExclusionComponent* Proxy) const
{
	if (!IsValid(Proxy) || !Proxy->bEnabled)
	{
		return nullptr;
	}

	if (UOceanBodyComponent* Explicit =
		Proxy->GetExplicitTargetWaterBody())
	{
		if (BodyContexts.Contains(Explicit)
			&& Explicit->BodyType != EOceanBodyType::River)
		{
			return Explicit;
		}

		return nullptr;
	}

	const FVector ProxyLocation = Proxy->GetComponentLocation();

	UOceanBodyComponent* BestBody = nullptr;
	float BestVerticalDistance = TNumericLimits<float>::Max();
	int32 BestPriority = TNumericLimits<int32>::Lowest();

	for (const TPair<TObjectPtr<UOceanBodyComponent>, FShipWaveBodyMaskContext>& Pair
		: BodyContexts)
	{
		UOceanBodyComponent* Body = Pair.Key;

		if (!IsValid(Body)
			|| Body->BodyType == EOceanBodyType::River
			|| !IsProxyInsideBodyXY(Proxy, Body))
		{
			continue;
		}

		const float VerticalDistance =
			FMath::Abs(ProxyLocation.Z - Body->GetComponentLocation().Z);

		if (VerticalDistance > Proxy->MaxAutoAssignVerticalDistance)
		{
			continue;
		}

		// Prefer the vertically nearest surface. Priority breaks close ties.
		const bool bCloser =
			VerticalDistance < BestVerticalDistance - 1.0f;

		const bool bPriorityWinsTie =
			FMath::IsNearlyEqual(
				VerticalDistance,
				BestVerticalDistance,
				1.0f)
			&& Body->Priority > BestPriority;

		if (bCloser || bPriorityWinsTie)
		{
			BestBody = Body;
			BestVerticalDistance = VerticalDistance;
			BestPriority = Body->Priority;
		}
	}

	return BestBody;
}

void UShipWaveMaskSubsystem::RebuildBodyMask(
	FShipWaveBodyMaskContext& Context)
{
	if (!StampMID
		|| !IsValid(Context.MaskRenderTarget)
		|| !IsValid(Context.WaterBody))
	{
		return;
	}

	UKismetRenderingLibrary::ClearRenderTarget2D(
		this,
		Context.MaskRenderTarget,
		FLinearColor::Black);

	for (auto It = Proxies.CreateIterator(); It; ++It)
	{
		UShipWaveExclusionComponent* Proxy = It->Get();

		if (!IsValid(Proxy))
		{
			It.RemoveCurrent();
			continue;
		}

		if (ResolveWaterBodyForProxy(Proxy) != Context.WaterBody)
		{
			continue;
		}

		FVector2D Center;
		FVector2D Forward;
		FVector2D Right;
		FVector4 Shape0;
		FVector4 Shape1;

		Proxy->GetProxyWorldData(
			Center,
			Forward,
			Right,
			Shape0,
			Shape1);

		if (Shape1.W <= 0.0f)
		{
			continue;
		}

		StampMID->SetVectorParameterValue(
			TEXT("MaskWorldOrigin"),
			FLinearColor(
				Context.WorldOrigin.X,
				Context.WorldOrigin.Y,
				0.0f,
				0.0f));

		StampMID->SetVectorParameterValue(
			TEXT("MaskWorldSize"),
			FLinearColor(
				Context.WorldSize.X,
				Context.WorldSize.Y,
				0.0f,
				0.0f));

		StampMID->SetVectorParameterValue(
			TEXT("ShipCenter"),
			FLinearColor(Center.X, Center.Y, 0.0f, 0.0f));

		StampMID->SetVectorParameterValue(
			TEXT("ShipForward"),
			FLinearColor(Forward.X, Forward.Y, 0.0f, 0.0f));

		StampMID->SetVectorParameterValue(
			TEXT("ShipRight"),
			FLinearColor(Right.X, Right.Y, 0.0f, 0.0f));

		StampMID->SetVectorParameterValue(
			TEXT("ShapeParams0"),
			FLinearColor(
				Shape0.X,
				Shape0.Y,
				Shape0.Z,
				Shape0.W));

		StampMID->SetVectorParameterValue(
			TEXT("ShapeParams1"),
			FLinearColor(
				Shape1.X,
				Shape1.Y,
				Shape1.Z,
				Shape1.W));

		UKismetRenderingLibrary::DrawMaterialToRenderTarget(
			this,
			Context.MaskRenderTarget,
			StampMID);
	}
}

void UShipWaveMaskSubsystem::PushContextToMID(
	FShipWaveBodyMaskContext& Context)
{
	if (!IsValid(Context.OceanMID)
		|| !IsValid(Context.MaskRenderTarget))
	{
		return;
	}

	Context.OceanMID->SetTextureParameterValue(
		TEXT("ShipWaveMaskTexture"),
		Context.MaskRenderTarget);

	Context.OceanMID->SetVectorParameterValue(
		TEXT("ShipMaskWorldOrigin"),
		FLinearColor(
			Context.WorldOrigin.X,
			Context.WorldOrigin.Y,
			0.0f,
			0.0f));

	Context.OceanMID->SetVectorParameterValue(
		TEXT("ShipMaskWorldSize"),
		FLinearColor(
			Context.WorldSize.X,
			Context.WorldSize.Y,
			0.0f,
			0.0f));

	Context.OceanMID->SetScalarParameterValue(
		TEXT("ShipWaveMaskEnabled"),
		StampMID ? 1.0f : 0.0f);
}

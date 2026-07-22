// Copyright James Joslin. All Rights Reserved.

#include "ShipWaveMaskSubsystem.h"
#include "../Components/ShipWaveExclusionComponent.h"
#include "../Components/OceanBodyComponent.h"
#include "WaveParameterSubsystem.h"

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
	BodyToNetwork.Empty();
	WaterConnections.Empty();
	RegisteredSurfaces.Empty();
	NetworkContexts.Empty();
	BodyContexts.Empty();
	DeformationStampMID = nullptr;
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
	for (TPair<FGuid, FShipWaveNetworkContext>& Pair : NetworkContexts)
	{
		EnsureNetworkResources(Pair.Value);
		UpdateNetworkMapping(Pair.Value);
		PushNetworkToMIDs(Pair.Value);
	}
	RebuildNetworkTopology();

	UE_LOG(
		LogTemp,
		Warning,
		TEXT(
			"ShipWaveMask: Configured | StampMaterial=%s | "
			"DefaultResolution=%d | RegisteredBodies=%d"
		),
		*GetNameSafe(StampMaterial),
		DefaultMaskResolution,
		BodyContexts.Num()
	);
}

void UShipWaveMaskSubsystem::ConfigureDeformationField(
	UMaterialInterface* DeformationStampMaterial,
	int32 FieldResolution,
	float FieldWorldSize)
{
	DefaultFieldResolution = FMath::Clamp(FieldResolution, 128, 4096);
	DefaultFieldWorldSize = FMath::Max(FieldWorldSize, 1000.0f);
	DeformationStampMID = DeformationStampMaterial
		? UMaterialInstanceDynamic::Create(DeformationStampMaterial, this)
		: nullptr;

	RebuildNetworkTopology();
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

	if (TObjectPtr<UMaterialInstanceDynamic>* PreviousMID =
		RegisteredSurfaces.Find(WaterBody))
	{
		if (IsValid(*PreviousMID) && *PreviousMID != OceanMID)
		{
			(*PreviousMID)->SetScalarParameterValue(
				TEXT("ShipWaveMaskEnabled"), 0.0f);
			(*PreviousMID)->SetScalarParameterValue(
				TEXT("ShipWaveFieldEnabled"), 0.0f);
		}
	}
	if (!WaterBody->SurfaceNetworkId.IsValid())
	{
		WaterBody->SurfaceNetworkId = FGuid::NewGuid();
	}
	for (const TPair<TObjectPtr<UOceanBodyComponent>,
		TObjectPtr<UMaterialInstanceDynamic>>& Pair : RegisteredSurfaces)
	{
		if (Pair.Key != WaterBody && IsValid(Pair.Key)
			&& Pair.Key->SurfaceNetworkId == WaterBody->SurfaceNetworkId)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("ShipWaveMask: duplicate SurfaceNetworkId on '%s'; "
					"assigning a new standalone network identity."),
				*GetNameSafe(WaterBody->GetOwner()));
			WaterBody->SurfaceNetworkId = FGuid::NewGuid();
			break;
		}
	}
	RegisteredSurfaces.Add(WaterBody, OceanMID);

	// Retain the old whole-body R8 mask for flat-water compatibility. The
	// rolling network field below is authoritative and also supports rivers.
	FShipWaveBodyMaskContext* LegacyContext = nullptr;
	if (WaterBody->BodyType != EOceanBodyType::River)
	{
		LegacyContext = &BodyContexts.FindOrAdd(WaterBody);
		LegacyContext->WaterBody = WaterBody;
		LegacyContext->OceanMID = OceanMID;
		LegacyContext->Resolution = Resolution > 0
			? FMath::Clamp(Resolution, 64, 4096)
			: DefaultMaskResolution;

		UpdateContextMapping(*LegacyContext);
		EnsureContextResources(*LegacyContext);
		PushContextToMID(*LegacyContext);
	}
	else
	{
		BodyContexts.Remove(WaterBody);
	}

	RebuildNetworkTopology();

	UE_LOG(
		LogTemp,
		Warning,
		TEXT(
			"ShipWaveMask: Registered body | "
			"Actor=%s | Component=%s | MID=%s | "
			"Extent=(%.0f, %.0f) | WorldSize=(%.0f, %.0f) | "
			"Resolution=%d | RT=%s | Configured=%s"
		),
		*GetNameSafe(WaterBody->GetOwner()),
		*GetNameSafe(WaterBody),
		*GetNameSafe(OceanMID),
		WaterBody->Extent.X,
		WaterBody->Extent.Y,
		LegacyContext ? LegacyContext->WorldSize.X : DefaultFieldWorldSize,
		LegacyContext ? LegacyContext->WorldSize.Y : DefaultFieldWorldSize,
		LegacyContext ? LegacyContext->Resolution : DefaultFieldResolution,
		LegacyContext ? *GetNameSafe(LegacyContext->MaskRenderTarget) : TEXT("NetworkOnly"),
		StampMID ? TEXT("Yes") : TEXT("No")
	);
}

void UShipWaveMaskSubsystem::UnregisterWaterBody(
	UOceanBodyComponent* WaterBody)
{
	if (!WaterBody)
	{
		return;
	}

	UMaterialInstanceDynamic* PreviousMID = RegisteredSurfaces.FindRef(WaterBody);
	if (FShipWaveBodyMaskContext* Context = BodyContexts.Find(WaterBody))
	{
		if (!PreviousMID)
		{
			PreviousMID = Context->OceanMID;
		}
		// Leave the MID in a safe full-wave state if it still exists.
		if (IsValid(Context->OceanMID))
		{
			Context->OceanMID->SetScalarParameterValue(
				TEXT("ShipWaveMaskEnabled"), 0.0f);
		}
	}
	if (IsValid(PreviousMID))
	{
		PreviousMID->SetScalarParameterValue(
			TEXT("ShipWaveMaskEnabled"), 0.0f);
		PreviousMID->SetScalarParameterValue(
			TEXT("ShipWaveFieldEnabled"), 0.0f);
	}

	BodyContexts.Remove(WaterBody);
	RegisteredSurfaces.Remove(WaterBody);
	for (auto It = WaterConnections.CreateIterator(); It; ++It)
	{
		if (It.Value().SourceBody == WaterBody
			|| It.Value().TargetBody == WaterBody)
		{
			It.RemoveCurrent();
		}
	}
	RebuildNetworkTopology();
}

void UShipWaveMaskSubsystem::RegisterWaterConnection(
	const FGuid& ConnectionId,
	UOceanBodyComponent* SourceBody,
	UOceanBodyComponent* TargetBody,
	UMaterialInstanceDynamic* JunctionMID)
{
	if (!ConnectionId.IsValid() || !IsValid(SourceBody)
		|| !IsValid(TargetBody) || !IsValid(JunctionMID))
	{
		return;
	}

	FShipWaveConnectionContext& Connection =
		WaterConnections.FindOrAdd(ConnectionId);
	Connection.ConnectionId = ConnectionId;
	Connection.SourceBody = SourceBody;
	Connection.TargetBody = TargetBody;
	Connection.JunctionMID = JunctionMID;
	RebuildNetworkTopology();
}

void UShipWaveMaskSubsystem::UnregisterWaterConnection(
	const FGuid& ConnectionId)
{
	if (FShipWaveConnectionContext* Connection =
		WaterConnections.Find(ConnectionId))
	{
		if (IsValid(Connection->JunctionMID))
		{
			Connection->JunctionMID->SetScalarParameterValue(
				TEXT("ShipWaveMaskEnabled"), 0.0f);
			Connection->JunctionMID->SetScalarParameterValue(
				TEXT("ShipWaveFieldEnabled"), 0.0f);
		}
		WaterConnections.Remove(ConnectionId);
		RebuildNetworkTopology();
	}
}

static bool IsGuidBefore(const FGuid& Left, const FGuid& Right)
{
	if (Left.A != Right.A) return Left.A < Right.A;
	if (Left.B != Right.B) return Left.B < Right.B;
	if (Left.C != Right.C) return Left.C < Right.C;
	return Left.D < Right.D;
}

void UShipWaveMaskSubsystem::RebuildNetworkTopology()
{
	// Remove dead registrations first. A connection remains pending if both
	// endpoint components are alive but one has not registered its MID yet.
	for (auto It = RegisteredSurfaces.CreateIterator(); It; ++It)
	{
		if (!IsValid(It.Key()) || !IsValid(It.Value()))
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = WaterConnections.CreateIterator(); It; ++It)
	{
		const FShipWaveConnectionContext& Connection = It.Value();
		if (!IsValid(Connection.SourceBody)
			|| !IsValid(Connection.TargetBody)
			|| !IsValid(Connection.JunctionMID))
		{
			It.RemoveCurrent();
		}
	}

	TArray<TObjectPtr<UOceanBodyComponent>> Bodies;
	RegisteredSurfaces.GenerateKeyArray(Bodies);
	TMap<TObjectPtr<UOceanBodyComponent>, int32> BodyIndices;
	TArray<int32> Parents;
	Parents.SetNumUninitialized(Bodies.Num());
	for (int32 Index = 0; Index < Bodies.Num(); ++Index)
	{
		BodyIndices.Add(Bodies[Index], Index);
		Parents[Index] = Index;
	}

	auto FindRoot = [&Parents](int32 Index)
	{
		while (Parents[Index] != Index)
		{
			Index = Parents[Index];
		}
		return Index;
	};
	for (const TPair<FGuid, FShipWaveConnectionContext>& Pair : WaterConnections)
	{
		const int32* SourceIndex = BodyIndices.Find(Pair.Value.SourceBody);
		const int32* TargetIndex = BodyIndices.Find(Pair.Value.TargetBody);
		if (!SourceIndex || !TargetIndex)
		{
			continue;
		}
		const int32 SourceRoot = FindRoot(*SourceIndex);
		const int32 TargetRoot = FindRoot(*TargetIndex);
		if (SourceRoot != TargetRoot)
		{
			Parents[TargetRoot] = SourceRoot;
		}
	}

	TMap<int32, TArray<TObjectPtr<UOceanBodyComponent>>> Components;
	for (int32 Index = 0; Index < Bodies.Num(); ++Index)
	{
		Components.FindOrAdd(FindRoot(Index)).Add(Bodies[Index]);
	}

	TMap<FGuid, FShipWaveNetworkContext> PreviousContexts =
		MoveTemp(NetworkContexts);
	NetworkContexts.Reset();
	BodyToNetwork.Reset();

	for (TPair<int32, TArray<TObjectPtr<UOceanBodyComponent>>>& Component
		: Components)
	{
		FGuid NetworkId;
		for (UOceanBodyComponent* Body : Component.Value)
		{
			if (!Body->SurfaceNetworkId.IsValid())
			{
				Body->SurfaceNetworkId = FGuid::NewGuid();
			}
			if (!NetworkId.IsValid()
				|| IsGuidBefore(Body->SurfaceNetworkId, NetworkId))
			{
				NetworkId = Body->SurfaceNetworkId;
			}
		}
		if (!NetworkId.IsValid())
		{
			continue;
		}

		FShipWaveNetworkContext Context;
		if (FShipWaveNetworkContext* Previous = PreviousContexts.Find(NetworkId))
		{
			Context = MoveTemp(*Previous);
		}
		Context.NetworkId = NetworkId;
		Context.Resolution = DefaultFieldResolution;
		Context.WorldSize = FVector2D(
			DefaultFieldWorldSize, DefaultFieldWorldSize);
		Context.WaterBodies.Reset();
		Context.SurfaceMIDs.Reset();

		for (UOceanBodyComponent* Body : Component.Value)
		{
			Context.WaterBodies.Add(Body);
			Context.SurfaceMIDs.AddUnique(RegisteredSurfaces.FindRef(Body));
			BodyToNetwork.Add(Body, NetworkId);
		}
		for (const TPair<FGuid, FShipWaveConnectionContext>& Pair
			: WaterConnections)
		{
			const FShipWaveConnectionContext& Connection = Pair.Value;
			if (Component.Value.Contains(Connection.SourceBody)
				&& Component.Value.Contains(Connection.TargetBody))
			{
				Context.SurfaceMIDs.AddUnique(Connection.JunctionMID);
			}
		}

		EnsureNetworkResources(Context);
		UpdateNetworkMapping(Context);
		PushNetworkToMIDs(Context);
		NetworkContexts.Add(NetworkId, MoveTemp(Context));
	}
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
	if (!StampMID && !DeformationStampMID)
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
		if (BodyToNetwork.Contains(Context.WaterBody))
		{
			// The rolling network target is authoritative, even for a network
			// containing only this one flat water body.
			continue;
		}

		UpdateContextMapping(Context);
		EnsureContextResources(Context);
		RebuildBodyMask(Context);
		PushContextToMID(Context);
	}

	for (auto It = NetworkContexts.CreateIterator(); It; ++It)
	{
		FShipWaveNetworkContext& Context = It.Value();
		Context.SurfaceMIDs.RemoveAll([](UMaterialInstanceDynamic* MID)
		{
			return !IsValid(MID);
		});
		Context.WaterBodies.RemoveAll([](UOceanBodyComponent* Body)
		{
			return !IsValid(Body);
		});
		if (Context.SurfaceMIDs.IsEmpty() || Context.WaterBodies.IsEmpty())
		{
			It.RemoveCurrent();
			continue;
		}
		UpdateNetworkMapping(Context);
		EnsureNetworkResources(Context);
		RebuildNetworkFields(Context);
		PushNetworkToMIDs(Context);
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

void UShipWaveMaskSubsystem::EnsureNetworkResources(
	FShipWaveNetworkContext& Context)
{
	const int32 Resolution = FMath::Clamp(Context.Resolution, 128, 4096);
	auto EnsureTarget = [this, Resolution](
		TObjectPtr<UTextureRenderTarget2D>& Target,
		ETextureRenderTargetFormat Format)
	{
		if (IsValid(Target)
			&& Target->SizeX == Resolution
			&& Target->SizeY == Resolution
			&& Target->RenderTargetFormat == Format)
		{
			return;
		}

		Target = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
		Target->RenderTargetFormat = Format;
		Target->Filter = TF_Bilinear;
		Target->AddressX = TA_Clamp;
		Target->AddressY = TA_Clamp;
		Target->ClearColor = FLinearColor::Black;
		Target->InitAutoFormat(Resolution, Resolution);
		Target->UpdateResourceImmediate(true);
	};

	if (StampMID)
	{
		EnsureTarget(Context.SuppressionRenderTarget, RTF_R8);
	}
	if (DeformationStampMID)
	{
		EnsureTarget(Context.DeformationRenderTarget, RTF_RGBA16f);
	}
}

bool UShipWaveMaskSubsystem::DoesProxyAffectNetwork(
	const UShipWaveExclusionComponent* Proxy,
	const FShipWaveNetworkContext& Context) const
{
	if (!IsValid(Proxy) || !Proxy->bEnabled)
	{
		return false;
	}

	if (UOceanBodyComponent* Explicit = Proxy->GetExplicitTargetWaterBody())
	{
		return Context.WaterBodies.Contains(Explicit);
	}

	if (const UWorld* World = GetWorld())
	{
		if (const UWaveParameterSubsystem* Waves =
			World->GetSubsystem<UWaveParameterSubsystem>())
		{
			const FVector Location = Proxy->GetComponentLocation();
			if (const FWaterBodyEntry* Body = Waves->FindWaterBodyAt(
				FVector2D(Location.X, Location.Y)))
			{
				return Context.WaterBodies.Contains(Body->Owner.Get());
			}
		}
	}

	return false;
}

void UShipWaveMaskSubsystem::UpdateNetworkMapping(
	FShipWaveNetworkContext& Context)
{
	FVector2D Center = FVector2D::ZeroVector;
	int32 Count = 0;
	for (const TWeakObjectPtr<UShipWaveExclusionComponent>& WeakProxy : Proxies)
	{
		const UShipWaveExclusionComponent* Proxy = WeakProxy.Get();
		if (DoesProxyAffectNetwork(Proxy, Context))
		{
			const FVector Location = Proxy->GetComponentLocation();
			Center += FVector2D(Location.X, Location.Y);
			++Count;
		}
	}
	if (Count > 0)
	{
		Center /= static_cast<double>(Count);
	}
	else
	{
		for (const UOceanBodyComponent* Body : Context.WaterBodies)
		{
			if (IsValid(Body))
			{
				const FVector Location = Body->GetComponentLocation();
				Center += FVector2D(Location.X, Location.Y);
				++Count;
			}
		}
		if (Count > 0)
		{
			Center /= static_cast<double>(Count);
		}
	}

	Context.WorldSize = FVector2D(
		FMath::Max(Context.WorldSize.X, 1000.0),
		FMath::Max(Context.WorldSize.Y, 1000.0));
	const double TexelX = Context.WorldSize.X / FMath::Max(Context.Resolution, 1);
	const double TexelY = Context.WorldSize.Y / FMath::Max(Context.Resolution, 1);
	const FVector2D DesiredOrigin = Center - Context.WorldSize * 0.5;
	Context.WorldOrigin = FVector2D(
		FMath::FloorToDouble(DesiredOrigin.X / TexelX) * TexelX,
		FMath::FloorToDouble(DesiredOrigin.Y / TexelY) * TexelY);
}

static void PushNetworkStampParameters(
	UMaterialInstanceDynamic& MID,
	const FShipWaveNetworkContext& Context,
	const UShipWaveExclusionComponent& Proxy,
	bool bDeformation)
{
	FVector2D Center;
	FVector2D Forward;
	FVector2D Right;
	FVector4 Shape0;
	FVector4 Shape1;
	Proxy.GetProxyWorldData(Center, Forward, Right, Shape0, Shape1);

	MID.SetVectorParameterValue(TEXT("MaskWorldOrigin"), FLinearColor(
		Context.WorldOrigin.X, Context.WorldOrigin.Y, 0.0, 0.0));
	MID.SetVectorParameterValue(TEXT("MaskWorldSize"), FLinearColor(
		Context.WorldSize.X, Context.WorldSize.Y, 0.0, 0.0));
	MID.SetVectorParameterValue(TEXT("ShipCenter"), FLinearColor(
		Center.X, Center.Y, 0.0, 0.0));
	MID.SetVectorParameterValue(TEXT("ShipForward"), FLinearColor(
		Forward.X, Forward.Y, 0.0, 0.0));
	MID.SetVectorParameterValue(TEXT("ShipRight"), FLinearColor(
		Right.X, Right.Y, 0.0, 0.0));
	MID.SetVectorParameterValue(TEXT("ShapeParams0"), FLinearColor(
		Shape0.X, Shape0.Y, Shape0.Z, Shape0.W));
	MID.SetVectorParameterValue(TEXT("ShapeParams1"), FLinearColor(
		Shape1.X, Shape1.Y, Shape1.Z, Shape1.W));
	if (bDeformation)
	{
		const FVector4 Deformation = Proxy.GetDeformationParams();
		MID.SetVectorParameterValue(TEXT("DeformationParams"), FLinearColor(
			Deformation.X, Deformation.Y, Deformation.Z, Deformation.W));
	}
}

void UShipWaveMaskSubsystem::RebuildNetworkFields(
	FShipWaveNetworkContext& Context)
{
	if (!IsValid(Context.SuppressionRenderTarget)
		&& !IsValid(Context.DeformationRenderTarget))
	{
		return;
	}

	if (IsValid(Context.SuppressionRenderTarget))
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(
			this, Context.SuppressionRenderTarget, FLinearColor::Black);
	}
	if (IsValid(Context.DeformationRenderTarget))
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(
			this, Context.DeformationRenderTarget, FLinearColor::Black);
	}

	for (const TWeakObjectPtr<UShipWaveExclusionComponent>& WeakProxy : Proxies)
	{
		UShipWaveExclusionComponent* Proxy = WeakProxy.Get();
		if (!DoesProxyAffectNetwork(Proxy, Context))
		{
			continue;
		}

		if (StampMID && IsValid(Context.SuppressionRenderTarget))
		{
			PushNetworkStampParameters(*StampMID, Context, *Proxy, false);
			UKismetRenderingLibrary::DrawMaterialToRenderTarget(
				this, Context.SuppressionRenderTarget, StampMID);
		}
		if (DeformationStampMID && IsValid(Context.DeformationRenderTarget))
		{
			PushNetworkStampParameters(*DeformationStampMID, Context, *Proxy, true);
			UKismetRenderingLibrary::DrawMaterialToRenderTarget(
				this, Context.DeformationRenderTarget, DeformationStampMID);
		}
	}
}

void UShipWaveMaskSubsystem::PushNetworkToMIDs(
	FShipWaveNetworkContext& Context)
{
	for (UMaterialInstanceDynamic* MID : Context.SurfaceMIDs)
	{
		if (!IsValid(MID))
		{
			continue;
		}
		if (IsValid(Context.SuppressionRenderTarget))
		{
			MID->SetTextureParameterValue(
				TEXT("ShipWaveMaskTexture"), Context.SuppressionRenderTarget);
			MID->SetTextureParameterValue(
				TEXT("ShipWaveSuppressionTexture"), Context.SuppressionRenderTarget);
		}
		if (IsValid(Context.DeformationRenderTarget))
		{
			MID->SetTextureParameterValue(
				TEXT("ShipWaveDeformationTexture"), Context.DeformationRenderTarget);
		}
		MID->SetVectorParameterValue(TEXT("ShipMaskWorldOrigin"), FLinearColor(
			Context.WorldOrigin.X, Context.WorldOrigin.Y, 0.0, 0.0));
		MID->SetVectorParameterValue(TEXT("ShipMaskWorldSize"), FLinearColor(
			Context.WorldSize.X, Context.WorldSize.Y, 0.0, 0.0));
		MID->SetVectorParameterValue(TEXT("ShipFieldWorldOrigin"), FLinearColor(
			Context.WorldOrigin.X, Context.WorldOrigin.Y, 0.0, 0.0));
		MID->SetVectorParameterValue(TEXT("ShipFieldWorldSize"), FLinearColor(
			Context.WorldSize.X, Context.WorldSize.Y, 0.0, 0.0));
		MID->SetScalarParameterValue(
			TEXT("ShipWaveMaskEnabled"),
			(StampMID && IsValid(Context.SuppressionRenderTarget)) ? 1.0f : 0.0f);
		MID->SetScalarParameterValue(
			TEXT("ShipWaveFieldEnabled"),
			(DeformationStampMID && IsValid(Context.DeformationRenderTarget))
				? 1.0f : 0.0f);
	}
}

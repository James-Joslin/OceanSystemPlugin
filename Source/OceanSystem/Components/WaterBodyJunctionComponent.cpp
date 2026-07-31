// Copyright James Joslin. All Rights Reserved.

#include "WaterBodyJunctionComponent.h"

#include "OceanBodyComponent.h"
#include "TiledWaterMeshComponent.h"
#include "../Subsystem/WaveParameterSubsystem.h"
#include "../Subsystem/ShipWaveMaskSubsystem.h"
#include "Components/SplineComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"

UWaterBodyJunctionComponent::UWaterBodyJunctionComponent(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCastShadow(false);
	bUseAsyncCooking = true;
}

bool UWaterBodyJunctionComponent::ConfigureJunction(
	UOceanBodyComponent* InSourceBody,
	USplineComponent* InRiverSpline,
	float InRiverWidth,
	const FWaterBodyConnectionConfig& InConfig)
{
	SourceBody = InSourceBody;
	RiverSpline = InRiverSpline;
	RiverWidth = FMath::Max(InRiverWidth, 10.0f);
	Config = InConfig;
	TargetBody = InConfig.TargetBody;
	return RebuildJunction();
}

bool UWaterBodyJunctionComponent::RebuildJunction()
{
	ClearMeshSection(0);
	bJunctionValid = false;

	if (!Config.IsUsable() || !IsValid(SourceBody) || !IsValid(TargetBody)
		|| !IsValid(RiverSpline)
		|| SourceBody->BodyType != EOceanBodyType::River
		|| TargetBody->BodyType == EOceanBodyType::River
		|| RiverSpline->IsClosedLoop())
	{
		ClearJunction();
		return false;
	}

	if (!Config.ConnectionId.IsValid())
	{
		Config.ConnectionId = FGuid::NewGuid();
	}
	if (!Config.NetworkId.IsValid())
	{
		Config.NetworkId = Config.ConnectionId;
	}

	UMaterialInterface* ParentMaterial = Config.JunctionMaterial.IsNull()
		? SourceBody->BaseMaterial.LoadSynchronous()
		: Config.JunctionMaterial.LoadSynchronous();
	if (!ParentMaterial)
	{
		UE_LOG(LogTemp, Error,
			TEXT("Water junction '%s' has no source material."), *GetName());
		ClearJunction();
		return false;
	}

	if (!JunctionMID || JunctionMID->Parent != ParentMaterial)
	{
		JunctionMID = UMaterialInstanceDynamic::Create(
			ParentMaterial, this, TEXT("WaterJunctionMID"));
	}

	if (!JunctionMID || !BuildGeometry() || !RegisterTargetCutout())
	{
		ClearJunction();
		return false;
	}

	SetMaterial(0, JunctionMID);
	RegisterSubsystemBindings();
	bJunctionValid = true;
	SetVisibility(true, true);
	return true;
}

void UWaterBodyJunctionComponent::ClearJunction()
{
	if (UWorld* World = GetWorld())
	{
		if (UWaveParameterSubsystem* Waves =
			World->GetSubsystem<UWaveParameterSubsystem>())
		{
			Waves->UnregisterWaterConnection(Config.ConnectionId);
		}
		if (UShipWaveMaskSubsystem* ShipFields =
			World->GetSubsystem<UShipWaveMaskSubsystem>())
		{
			ShipFields->UnregisterWaterConnection(Config.ConnectionId);
		}
	}
	UnregisterTargetCutout();
	ClearAllMeshSections();
	FootprintWorld.Empty();
	bJunctionValid = false;
	SetVisibility(false, true);
}

void UWaterBodyJunctionComponent::OnUnregister()
{
	ClearJunction();
	Super::OnUnregister();
}

bool UWaterBodyJunctionComponent::BuildGeometry()
{
	const float SplineLength = RiverSpline->GetSplineLength();
	const float EndpointDistance = Config.Endpoint == EWaterConnectionEndpoint::End
		? SplineLength
		: 0.0f;
	const FVector WorldStart = RiverSpline->GetLocationAtDistanceAlongSpline(
		EndpointDistance, ESplineCoordinateSpace::World);
	FVector WorldDirection = RiverSpline->GetDirectionAtDistanceAlongSpline(
		EndpointDistance, ESplineCoordinateSpace::World).GetSafeNormal2D();
	if (Config.Endpoint == EWaterConnectionEndpoint::Start)
	{
		WorldDirection *= -1.0f;
	}
	if (WorldDirection.IsNearlyZero())
	{
		return false;
	}

	const FVector WorldRight = FVector::CrossProduct(
		FVector::UpVector, WorldDirection).GetSafeNormal();
	const float StartHalfWidth = RiverWidth * 0.5f;
	const float EndHalfWidth = StartHalfWidth * FMath::Max(Config.MouthWidthScale, 0.25f);
	const float Length = FMath::Max(Config.BlendLength, 10.0f);
	const float TargetZ = TargetBody->GetComponentLocation().Z;
	const int32 AlongCount = FMath::Clamp(Config.LengthSubdivisions, 2, 128);
	const int32 AcrossCount = FMath::Clamp(Config.WidthSubdivisions, 1, 64);

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	Vertices.Reserve((AlongCount + 1) * (AcrossCount + 1));

	const FTransform ToLocal = GetComponentTransform();
	for (int32 AlongIndex = 0; AlongIndex <= AlongCount; ++AlongIndex)
	{
		const float T = static_cast<float>(AlongIndex) / AlongCount;
		const float HalfWidth = FMath::Lerp(StartHalfWidth, EndHalfWidth, T);
		const FVector Center = WorldStart + WorldDirection * (Length * T);
		const float BaseZ = FMath::Lerp(WorldStart.Z, TargetZ, T);

		for (int32 AcrossIndex = 0; AcrossIndex <= AcrossCount; ++AcrossIndex)
		{
			const float U = static_cast<float>(AcrossIndex) / AcrossCount;
			FVector WorldPoint = Center + WorldRight * FMath::Lerp(-HalfWidth, HalfWidth, U);
			WorldPoint.Z = BaseZ;
			Vertices.Add(ToLocal.InverseTransformPosition(WorldPoint));
			Normals.Add(ToLocal.InverseTransformVectorNoScale(FVector::UpVector));
			UVs.Emplace(T, U);
			Colors.Add(FColor::White);
			Tangents.Emplace(
				ToLocal.InverseTransformVectorNoScale(WorldDirection), false);
		}
	}

	const int32 RowSize = AcrossCount + 1;
	Triangles.Reserve(AlongCount * AcrossCount * 6);
	for (int32 AlongIndex = 0; AlongIndex < AlongCount; ++AlongIndex)
	{
		for (int32 AcrossIndex = 0; AcrossIndex < AcrossCount; ++AcrossIndex)
		{
			const int32 A = AlongIndex * RowSize + AcrossIndex;
			const int32 B = A + 1;
			const int32 C = A + RowSize;
			const int32 D = C + 1;
			Triangles.Append({ A, D, B, A, C, D });
		}
	}

	CreateMeshSection(
		0, Vertices, Triangles, Normals, UVs, Colors, Tangents,
		/*bCreateCollision=*/false);

	// The cutout consumes this exact perimeter. Keeping every tessellated edge
	// vertex in the loop prevents WPO cracks caused by a long target edge being
	// displaced differently from the junction's subdivided boundary.
	FootprintWorld.Reset(2 * (AlongCount + AcrossCount));
	for (int32 AcrossIndex = 0; AcrossIndex <= AcrossCount; ++AcrossIndex)
	{
		const int32 VertexIndex = AcrossIndex;
		FootprintWorld.Add(ToLocal.TransformPosition(Vertices[VertexIndex]));
	}
	for (int32 AlongIndex = 1; AlongIndex <= AlongCount; ++AlongIndex)
	{
		const int32 VertexIndex = AlongIndex * RowSize + AcrossCount;
		FootprintWorld.Add(ToLocal.TransformPosition(Vertices[VertexIndex]));
	}
	for (int32 AcrossIndex = AcrossCount - 1; AcrossIndex >= 0; --AcrossIndex)
	{
		const int32 VertexIndex = AlongCount * RowSize + AcrossIndex;
		FootprintWorld.Add(ToLocal.TransformPosition(Vertices[VertexIndex]));
	}
	for (int32 AlongIndex = AlongCount - 1; AlongIndex > 0; --AlongIndex)
	{
		const int32 VertexIndex = AlongIndex * RowSize;
		FootprintWorld.Add(ToLocal.TransformPosition(Vertices[VertexIndex]));
	}

	float SourceAmplitude = 0.0f;
	for (const FGerstnerWaveLayer& Layer : SourceBody->WaveConfig.Layers)
	{
		SourceAmplitude += Layer.Amplitude;
	}
	float TargetAmplitude = 0.0f;
	for (const FGerstnerWaveLayer& Layer : TargetBody->WaveConfig.Layers)
	{
		TargetAmplitude += Layer.Amplitude;
	}
	const float RequiredVertical = FMath::Max(SourceAmplitude, TargetAmplitude)
		+ FMath::Abs(TargetZ - WorldStart.Z) + 500.0f;
	const float HorizontalExtent = FMath::Max(Length, EndHalfWidth * 2.0f);
	BoundsScale = FMath::Max(1.0f + RequiredVertical / FMath::Max(HorizontalExtent, 1.0f), 1.0f);
	UpdateBounds();

	UpdateMIDGeometryParameters(WorldStart, WorldDirection, WorldRight);
	return true;
}

bool UWaterBodyJunctionComponent::RegisterTargetCutout()
{
	UnregisterTargetCutout();
	if (AActor* TargetActor = TargetBody ? TargetBody->GetOwner() : nullptr)
	{
		TargetTiledMesh = TargetActor->FindComponentByClass<UTiledWaterMeshComponent>();
	}
	return TargetTiledMesh
		&& TargetTiledMesh->RegisterCutout(Config.ConnectionId, FootprintWorld);
}

void UWaterBodyJunctionComponent::UnregisterTargetCutout()
{
	if (TargetTiledMesh && Config.ConnectionId.IsValid())
	{
		TargetTiledMesh->UnregisterCutout(Config.ConnectionId);
	}
	TargetTiledMesh = nullptr;
}

void UWaterBodyJunctionComponent::RegisterSubsystemBindings()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	UWaveParameterSubsystem* Waves = World->GetSubsystem<UWaveParameterSubsystem>();
	if (!Waves)
	{
		return;
	}

	Waves->RegisterWaterConnection(SourceBody, Config, JunctionMID, this);
	const float SplineLength = RiverSpline->GetSplineLength();
	const float EndpointDistance = Config.Endpoint == EWaterConnectionEndpoint::End
		? SplineLength
		: 0.0f;
	const FVector WorldStart = RiverSpline->GetLocationAtDistanceAlongSpline(
		EndpointDistance, ESplineCoordinateSpace::World);
	FVector WorldDirection = RiverSpline->GetDirectionAtDistanceAlongSpline(
		EndpointDistance, ESplineCoordinateSpace::World).GetSafeNormal2D();
	if (Config.Endpoint == EWaterConnectionEndpoint::Start)
	{
		WorldDirection *= -1.0f;
	}
	const FVector WorldRight = FVector::CrossProduct(
		FVector::UpVector, WorldDirection).GetSafeNormal();
	Waves->UpdateWaterConnectionGeometry(
		Config.ConnectionId,
		WorldStart,
		WorldDirection,
		WorldRight,
		RiverWidth * 0.5f,
		RiverWidth * 0.5f * Config.MouthWidthScale,
		Config.BlendLength);

	if (UShipWaveMaskSubsystem* ShipFields =
		World->GetSubsystem<UShipWaveMaskSubsystem>())
	{
		ShipFields->RegisterWaterConnection(
			Config.ConnectionId, SourceBody, TargetBody, JunctionMID);
	}
}

void UWaterBodyJunctionComponent::UpdateMIDGeometryParameters(
	const FVector& WorldStart,
	const FVector& WorldDirection,
	const FVector& WorldRight)
{
	if (!JunctionMID)
	{
		return;
	}
	JunctionMID->SetVectorParameterValue(
		TEXT("ConnectionWorldStart"), FLinearColor(WorldStart));
	JunctionMID->SetVectorParameterValue(
		TEXT("ConnectionWorldDirection"), FLinearColor(WorldDirection));
	JunctionMID->SetVectorParameterValue(
		TEXT("ConnectionWorldRight"), FLinearColor(WorldRight));
	JunctionMID->SetScalarParameterValue(
		TEXT("ConnectionBlendLength"), Config.BlendLength);
	JunctionMID->SetScalarParameterValue(TEXT("ConnectionEnabled"), 1.0f);
	// Useful for dynamic/custom-node graphs. The intended production material
	// still has its static ConnectionMode switch compiled in the assigned
	// JunctionMaterial asset so ordinary water avoids the dual evaluation.
	JunctionMID->SetScalarParameterValue(TEXT("ConnectionMode"), 1.0f);
}

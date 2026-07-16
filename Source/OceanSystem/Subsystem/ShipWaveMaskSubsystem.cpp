// ShipWaveMaskSubsystem.cpp

#include "ShipWaveMaskSubsystem.h"
#include "../Components/ShipWaveExclusionComponent.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

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
    BoundOceanMIDs.Empty();
    StampMID = nullptr;
    MaskRenderTarget = nullptr;
    Super::Deinitialize();
}

TStatId UShipWaveMaskSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UShipWaveMaskSubsystem, STATGROUP_Tickables);
}

void UShipWaveMaskSubsystem::Configure(
    UMaterialInterface* StampMaterial,
    int32 Resolution,
    FVector2D WorldSize)
{
    if (!StampMaterial || !GetWorld())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ShipWaveMaskSubsystem: Configure requires a valid stamp material and world."));
        return;
    }

    MaskResolution = FMath::Clamp(Resolution, 64, 4096);
    MaskWorldSize.X = FMath::Max(WorldSize.X, 100.0f);
    MaskWorldSize.Y = FMath::Max(WorldSize.Y, 100.0f);

    MaskRenderTarget = NewObject<UTextureRenderTarget2D>(this);
    MaskRenderTarget->RenderTargetFormat = RTF_R8;
    MaskRenderTarget->Filter = TF_Bilinear;
    MaskRenderTarget->AddressX = TA_Clamp;
    MaskRenderTarget->AddressY = TA_Clamp;
    MaskRenderTarget->ClearColor = FLinearColor::Black;
    MaskRenderTarget->InitAutoFormat(MaskResolution, MaskResolution);
    MaskRenderTarget->UpdateResourceImmediate(true);

    StampMID = UMaterialInstanceDynamic::Create(StampMaterial, this);
    bConfigured = StampMID != nullptr;

    RefreshBoundMIDs();
}

void UShipWaveMaskSubsystem::SetMaskCenter(FVector2D NewCenter)
{
    MaskCenter = NewCenter;
}

void UShipWaveMaskSubsystem::BindOceanMID(UMaterialInstanceDynamic* OceanMID)
{
    if (!OceanMID)
    {
        return;
    }

    BoundOceanMIDs.AddUnique(OceanMID);
    RefreshBoundMIDs();
}

void UShipWaveMaskSubsystem::RegisterProxy(UShipWaveExclusionComponent* Proxy)
{
    if (Proxy)
    {
        Proxies.Add(Proxy);
    }
}

void UShipWaveMaskSubsystem::UnregisterProxy(UShipWaveExclusionComponent* Proxy)
{
    if (Proxy)
    {
        Proxies.Remove(Proxy);
    }
}

void UShipWaveMaskSubsystem::Tick(float DeltaTime)
{
    if (!bConfigured || !MaskRenderTarget || !StampMID || !GetWorld())
    {
        return;
    }

    if (bFollowPlayerCamera)
    {
        if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
        {
            if (APlayerCameraManager* Camera = PC->PlayerCameraManager)
            {
                const FVector CameraLocation = Camera->GetCameraLocation();
                MaskCenter = FVector2D(CameraLocation.X, CameraLocation.Y);
            }
        }
    }

    RebuildMask();
    RefreshBoundMIDs();
}

void UShipWaveMaskSubsystem::RebuildMask()
{
    UKismetRenderingLibrary::ClearRenderTarget2D(
        this,
        MaskRenderTarget,
        FLinearColor::Black);

    const FVector2D Origin = MaskCenter - MaskWorldSize * 0.5f;

    // This baseline implementation performs one additive full-screen draw per active proxy.
    // It is simple and reliable. For hundreds of ships, replace this with an instanced RDG pass.
    for (auto It = Proxies.CreateIterator(); It; ++It)
    {
        UShipWaveExclusionComponent* Proxy = It->Get();

        if (!IsValid(Proxy))
        {
            It.RemoveCurrent();
            continue;
        }

        FVector2D Center;
        FVector2D Forward;
        FVector2D Right;
        FVector4 Shape0;
        FVector4 Shape1;
        Proxy->GetProxyWorldData(Center, Forward, Right, Shape0, Shape1);

        if (Shape1.W <= 0.0f)
        {
            continue;
        }

        // Cull proxies whose broad bounds do not intersect this mask window.
        const float BroadRadius = FMath::Max(
            Shape0.X + Shape0.Y,
            FVector2D(Shape0.Z, Shape0.W).Size());

        const FVector2D ToCenter = Center - MaskCenter;
        if (FMath::Abs(ToCenter.X) > MaskWorldSize.X * 0.5f + BroadRadius ||
            FMath::Abs(ToCenter.Y) > MaskWorldSize.Y * 0.5f + BroadRadius)
        {
            continue;
        }

        StampMID->SetVectorParameterValue(
            TEXT("MaskWorldOrigin"),
            FLinearColor(Origin.X, Origin.Y, 0.0f, 0.0f));

        StampMID->SetVectorParameterValue(
            TEXT("MaskWorldSize"),
            FLinearColor(MaskWorldSize.X, MaskWorldSize.Y, 0.0f, 0.0f));

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
            FLinearColor(Shape0.X, Shape0.Y, Shape0.Z, Shape0.W));

        StampMID->SetVectorParameterValue(
            TEXT("ShapeParams1"),
            FLinearColor(Shape1.X, Shape1.Y, Shape1.Z, Shape1.W));

        UKismetRenderingLibrary::DrawMaterialToRenderTarget(
            this,
            MaskRenderTarget,
            StampMID);
    }
}

void UShipWaveMaskSubsystem::RefreshBoundMIDs()
{
    if (!MaskRenderTarget)
    {
        return;
    }

    const FVector2D Origin = MaskCenter - MaskWorldSize * 0.5f;

    for (int32 Index = BoundOceanMIDs.Num() - 1; Index >= 0; --Index)
    {
        UMaterialInstanceDynamic* MID = BoundOceanMIDs[Index];

        if (!IsValid(MID))
        {
            BoundOceanMIDs.RemoveAtSwap(Index);
            continue;
        }

        MID->SetTextureParameterValue(TEXT("ShipWaveMaskTexture"), MaskRenderTarget);
        MID->SetVectorParameterValue(
            TEXT("ShipMaskWorldOrigin"),
            FLinearColor(Origin.X, Origin.Y, 0.0f, 0.0f));
        MID->SetVectorParameterValue(
            TEXT("ShipMaskWorldSize"),
            FLinearColor(MaskWorldSize.X, MaskWorldSize.Y, 0.0f, 0.0f));
    }
}

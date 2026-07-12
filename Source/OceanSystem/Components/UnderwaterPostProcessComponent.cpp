// Copyright James Joslin. All Rights Reserved.

#include "UnderwaterPostProcessComponent.h"
#include "../Subsystem/WaveParameterSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"

// ===================================================================
// MID parameter contract — keep in sync with PP_Underwater
// ===================================================================

namespace UnderwaterPP
{
	/** 0 = at/above surface, 1 = fully submerged. The material should
		lerp(SceneColor, EffectColor, EffectStrength) itself — this is
		deterministic across volumes, unlike relying on blendable-weight
		parameter lerping. */
	static const FName EffectStrengthName(TEXT("EffectStrength"));

	/** World Z of the wave surface at the camera, per frame. Used for the
		per-pixel waterline mask (fog only below this plane). */
	static const FName SurfaceZName(TEXT("SurfaceZ"));

	/** How far the camera is below the surface (cm). Depth-based darkening. */
	static const FName CameraDepthName(TEXT("CameraDepth"));

	/** Exponential fog density. */
	static const FName FogDensityName(TEXT("FogDensity"));

	/** Fog / scatter colour. */
	static const FName WaterTintName(TEXT("WaterTint"));
}

// ===================================================================
// Lifecycle
// ===================================================================

UUnderwaterPostProcessComponent::UUnderwaterPostProcessComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// Run after cameras have updated for the frame, otherwise the surface
	// query uses last frame's camera position and the transition lags by
	// one frame when surfacing/submerging quickly.
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;

	ApplyGradeSettings();
}

void UUnderwaterPostProcessComponent::OnRegister()
{
	Super::OnRegister();

	// Same registration UPostProcessComponent performs internally: put
	// ourselves in the world's post-process volume list. The renderer
	// polls GetProperties() every frame from then on — BlendWeight 0
	// reports the volume disabled, so registering while above water
	// (or in the editor, where tick never runs) has no visual effect.
	if (UWorld* World = GetWorld())
	{
		World->InsertPostProcessVolume(this);
	}
}

void UUnderwaterPostProcessComponent::OnUnregister()
{
	if (UWorld* World = GetWorld())
	{
		World->RemovePostProcessVolume(this);
	}

	Super::OnUnregister();
}

void UUnderwaterPostProcessComponent::BeginPlay()
{
	Super::BeginPlay();

	// Properties may have been edited on the archetype/instance since the
	// constructor ran — refresh the settings block.
	ApplyGradeSettings();

	// Optional blendable for swirl distortion / fog / waterline masking.
	if (UMaterialInterface* Mat = UnderwaterMaterial.LoadSynchronous())
	{
		BlendableMID = UMaterialInstanceDynamic::Create(Mat, this);
		if (BlendableMID)
		{
			// Weight stays 1.0 — the material fades itself via
			// EffectStrength, and the volume's BlendWeight handles the
			// built-in settings. See UnderwaterPP::EffectStrengthName.
			Settings.WeightedBlendables.Array.Add(
				FWeightedBlendable(1.0f, BlendableMID));

			PushStaticMaterialParams();
		}
	}
}

void UUnderwaterPostProcessComponent::TickComponent(
	float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	float Alpha = 0.0f;
	float SurfaceZ = 0.0f;
	float CameraDepth = 0.0f;

	if (UWorld* World = GetWorld())
	{
		UWaveParameterSubsystem* Waves =
			World->GetSubsystem<UWaveParameterSubsystem>();

		APlayerCameraManager* PCM =
			UGameplayStatics::GetPlayerCameraManager(World, 0);

		if (Waves && PCM)
		{
			const FVector CamLoc = PCM->GetCameraLocation();

			// Full visual evaluation (all layers) — a single point per
			// frame, and physics LOD would flicker at the surface by the
			// amplitude of the excluded detail layers.
			if (Waves->GetFullWaveHeight(CamLoc, SurfaceZ))
			{
				CameraDepth = SurfaceZ - CamLoc.Z;
				Alpha = FMath::Clamp(
					CameraDepth / FMath::Max(BlendDistance, 1.0f),
					0.0f, 1.0f);
			}
		}
	}

	SetSubmergedAlpha(Alpha, SurfaceZ, CameraDepth);
}

#if WITH_EDITOR
void UUnderwaterPostProcessComponent::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	ApplyGradeSettings();
	PushStaticMaterialParams();
}
#endif

// ===================================================================
// IInterface_PostProcessVolume
// ===================================================================

bool UUnderwaterPostProcessComponent::EncompassesPoint(
	FVector Point, float SphereRadius, float* OutDistanceToPoint)
{
	// Unbound volume — encompasses everything, distance is always zero.
	if (OutDistanceToPoint)
	{
		*OutDistanceToPoint = 0.0f;
	}
	return true;
}

FPostProcessVolumeProperties UUnderwaterPostProcessComponent::GetProperties() const
{
	FPostProcessVolumeProperties Ret;
	Ret.bIsEnabled = BlendWeight > KINDA_SMALL_NUMBER;
	Ret.bIsUnbound = true;
	Ret.BlendRadius = 0.0f;
	Ret.BlendWeight = BlendWeight;
	Ret.Priority = Priority;
	Ret.Settings = &Settings;
	return Ret;
}

#if DEBUG_POST_PROCESS_VOLUME_ENABLE
FString UUnderwaterPostProcessComponent::GetDebugName() const
{
	return GetName();
}
#endif

// ===================================================================
// Internal
// ===================================================================

void UUnderwaterPostProcessComponent::ApplyGradeSettings()
{
	// The prototype recipe: darker, teal-shifted, desaturated, vignetted.
	// All built-in FPostProcessSettings — no material required. These five
	// fields are owned by the grade properties; everything else in the
	// exposed Settings struct is user territory.
	Settings.bOverride_AutoExposureBias = true;
	Settings.AutoExposureBias = ExposureBias;

	Settings.bOverride_ColorGain = true;
	Settings.ColorGain = FVector4(
		UnderwaterTint.R, UnderwaterTint.G, UnderwaterTint.B, 1.0f);

	Settings.bOverride_ColorSaturation = true;
	Settings.ColorSaturation = FVector4(
		Saturation, Saturation, Saturation, 1.0f);

	Settings.bOverride_VignetteIntensity = true;
	Settings.VignetteIntensity = VignetteIntensity;
}

void UUnderwaterPostProcessComponent::PushStaticMaterialParams()
{
	if (!BlendableMID)
	{
		return;
	}

	BlendableMID->SetScalarParameterValue(
		UnderwaterPP::FogDensityName, FogDensity);
	BlendableMID->SetVectorParameterValue(
		UnderwaterPP::WaterTintName, WaterTint);
}

void UUnderwaterPostProcessComponent::SetSubmergedAlpha(
	float Alpha, float SurfaceZ, float CameraDepth)
{
	BlendWeight = Alpha;

	if (BlendableMID && Alpha > KINDA_SMALL_NUMBER)
	{
		BlendableMID->SetScalarParameterValue(
			UnderwaterPP::EffectStrengthName, Alpha);
		BlendableMID->SetScalarParameterValue(
			UnderwaterPP::SurfaceZName, SurfaceZ);
		BlendableMID->SetScalarParameterValue(
			UnderwaterPP::CameraDepthName, CameraDepth);
	}
}
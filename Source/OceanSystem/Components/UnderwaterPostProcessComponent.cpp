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

	// -----------------------------------------------------------------
	// Default depth gradient:
	//   0 cm      — barely-there: minor darkening, whisper of tint
	//   150 cm    — light teal band begins
	//   1200 cm   — teal deepens, light starts to die
	//   4500 cm   — the deep look: dark, blue, vignetted
	// Depths beyond the last stop clamp to it. Reshape freely in the
	// Details panel; stops are re-sorted by depth on edit.
	// -----------------------------------------------------------------

	FUnderwaterGradeStop SurfaceStop;
	SurfaceStop.Depth = 0.0f;
	SurfaceStop.ExposureBias = -0.1f;
	SurfaceStop.Tint = FLinearColor(0.90f, 0.98f, 1.00f, 1.0f);
	SurfaceStop.Saturation = 0.95f;
	SurfaceStop.VignetteIntensity = 0.1f;
	DepthGrade.Add(SurfaceStop);

	FUnderwaterGradeStop ShallowStop;
	ShallowStop.Depth = 150.0f;
	ShallowStop.ExposureBias = -0.25f;
	ShallowStop.Tint = FLinearColor(0.70f, 0.95f, 0.95f, 1.0f); // light teal
	ShallowStop.Saturation = 0.85f;
	ShallowStop.VignetteIntensity = 0.2f;
	DepthGrade.Add(ShallowStop);

	FUnderwaterGradeStop MidStop;
	MidStop.Depth = 1200.0f;
	MidStop.ExposureBias = -0.6f;
	MidStop.Tint = FLinearColor(0.50f, 0.85f, 0.88f, 1.0f);
	MidStop.Saturation = 0.75f;
	MidStop.VignetteIntensity = 0.35f;
	DepthGrade.Add(MidStop);

	FUnderwaterGradeStop DeepStop;
	DeepStop.Depth = 4500.0f;
	DeepStop.ExposureBias = -1.0f;
	DeepStop.Tint = FLinearColor(0.35f, 0.75f, 0.80f, 1.0f); // the proven max-depth look
	DeepStop.Saturation = 0.65f;
	DeepStop.VignetteIntensity = 0.5f;
	DepthGrade.Add(DeepStop);

	ApplyGradeSettings(DepthGrade[0]);
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

	// Stops may have been edited on the archetype/instance since the
	// constructor ran — re-sort and refresh the settings block.
	SortDepthGrade();
	if (DepthGrade.Num() > 0)
	{
		ApplyGradeSettings(DepthGrade[0]);
	}

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

	// Depth axis: re-evaluate the gradient while submerged. Cheap — a
	// handful of lerps into a struct the renderer reads by pointer.
	if (Alpha > KINDA_SMALL_NUMBER)
	{
		ApplyGradeSettings(EvaluateDepthGrade(CameraDepth));
	}

	// Surface-crossing axis: weight + MID params.
	SetSubmergedAlpha(Alpha, SurfaceZ, CameraDepth);
}

#if WITH_EDITOR
void UUnderwaterPostProcessComponent::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	SortDepthGrade();
	if (DepthGrade.Num() > 0)
	{
		ApplyGradeSettings(DepthGrade[0]);
	}
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
// Depth gradient
// ===================================================================

FUnderwaterGradeStop UUnderwaterPostProcessComponent::EvaluateDepthGrade(
	float CameraDepth) const
{
	if (DepthGrade.Num() == 0)
	{
		return FUnderwaterGradeStop();
	}

	// Clamp outside the authored range.
	if (CameraDepth <= DepthGrade[0].Depth)
	{
		return DepthGrade[0];
	}
	if (CameraDepth >= DepthGrade.Last().Depth)
	{
		return DepthGrade.Last();
	}

	// Find the bracketing pair. Stop counts are tiny (typically 3-6);
	// a linear scan beats anything clever.
	int32 UpperIdx = 1;
	while (UpperIdx < DepthGrade.Num() - 1 &&
		DepthGrade[UpperIdx].Depth < CameraDepth)
	{
		++UpperIdx;
	}

	const FUnderwaterGradeStop& A = DepthGrade[UpperIdx - 1];
	const FUnderwaterGradeStop& B = DepthGrade[UpperIdx];

	const float Span = FMath::Max(B.Depth - A.Depth, 1.0f);
	float T = (CameraDepth - A.Depth) / Span;

	// Smoothstep so the gradient has no visible kinks at the stops.
	T = T * T * (3.0f - 2.0f * T);

	FUnderwaterGradeStop Out;
	Out.Depth = CameraDepth;
	Out.ExposureBias = FMath::Lerp(A.ExposureBias, B.ExposureBias, T);
	Out.Tint = FMath::Lerp(A.Tint, B.Tint, T);
	Out.Saturation = FMath::Lerp(A.Saturation, B.Saturation, T);
	Out.VignetteIntensity = FMath::Lerp(A.VignetteIntensity, B.VignetteIntensity, T);
	return Out;
}

void UUnderwaterPostProcessComponent::SortDepthGrade()
{
	DepthGrade.StableSort(
		[](const FUnderwaterGradeStop& A, const FUnderwaterGradeStop& B)
		{
			return A.Depth < B.Depth;
		});
}

// ===================================================================
// Internal
// ===================================================================

void UUnderwaterPostProcessComponent::ApplyGradeSettings(
	const FUnderwaterGradeStop& Stop)
{
	// These four fields are owned by the depth gradient; everything else
	// in the exposed Settings struct is user territory.
	Settings.bOverride_AutoExposureBias = true;
	Settings.AutoExposureBias = Stop.ExposureBias;

	Settings.bOverride_ColorGain = true;
	Settings.ColorGain = FVector4(
		Stop.Tint.R, Stop.Tint.G, Stop.Tint.B, 1.0f);

	Settings.bOverride_ColorSaturation = true;
	Settings.ColorSaturation = FVector4(
		Stop.Saturation, Stop.Saturation, Stop.Saturation, 1.0f);

	Settings.bOverride_VignetteIntensity = true;
	Settings.VignetteIntensity = Stop.VignetteIntensity;
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
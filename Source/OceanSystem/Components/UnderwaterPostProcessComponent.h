// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Engine/Scene.h"
#include "Interfaces/Interface_PostProcessVolume.h"
#include "UnderwaterPostProcessComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * One authored point on the underwater depth gradient. The component
 * smoothly interpolates between adjacent stops based on how far the
 * camera is below the water surface.
 */
USTRUCT(BlueprintType)
struct FUnderwaterGradeStop
{
	GENERATED_BODY()

	/** Camera depth below the surface where this stop fully applies (cm). */
	UPROPERTY(EditAnywhere, Category = "Grade", meta = (ClampMin = "0.0"))
	float Depth = 0.0f;

	/** Exposure bias at this depth. Negative = darker. */
	UPROPERTY(EditAnywhere, Category = "Grade")
	float ExposureBias = -0.1f;

	/** Colour gain at this depth — multiplies scene colour. */
	UPROPERTY(EditAnywhere, Category = "Grade")
	FLinearColor Tint = FLinearColor(0.9f, 0.98f, 1.0f, 1.0f);

	/** Saturation at this depth. 1 = unchanged. */
	UPROPERTY(EditAnywhere, Category = "Grade", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float Saturation = 0.95f;

	/** Vignette at this depth. */
	UPROPERTY(EditAnywhere, Category = "Grade", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VignetteIntensity = 0.1f;
};

/**
 * Unbound post-process volume that activates when the player camera goes
 * below the water surface.
 *
 * Implementation note — why this is NOT a UPostProcessComponent subclass:
 * UPostProcessComponent is MinimalAPI in the engine, so its virtual
 * overrides are not exported and it cannot be subclassed from a plugin
 * module (LNK2001 on OnRegister/OnUnregister/Serialize/PostInitProperties).
 * Instead this component implements IInterface_PostProcessVolume directly —
 * the same interface the renderer polls for APostProcessVolume — and
 * registers itself in the world's post-process volume list.
 *
 * Two independent axes:
 *   - SURFACE CROSSING: BlendWeight ramps 0 -> 1 over BlendDistance below
 *     the surface, fading the whole effect in/out cleanly at the waterline.
 *   - DEPTH GRADING: the *content* of the grade is evaluated from camera
 *     depth along DepthGrade — an authored gradient of stops (near-clear
 *     just under the surface, light teal in the mid band, dark blue at the
 *     deep end). Interpolation between adjacent stops is smoothstepped;
 *     depths beyond the last stop clamp to it. Add/move stops in the
 *     Details panel to reshape the gradient — no code changes.
 *
 * The grade stops own the exposure/gain/saturation/vignette fields in
 * Settings; everything else in the exposed Settings struct (e.g. depth of
 * field for underwater blur) is yours to override freely.
 *
 * Optional UnderwaterMaterial blendable for what built-ins can't do:
 * swirl distortion, depth fog, per-pixel waterline masking. The MID
 * receives EffectStrength / SurfaceZ / CameraDepth / FogDensity /
 * WaterTint (see UnderwaterPP namespace in the .cpp).
 *
 * Multiple bodies: every water actor carries one of these with identical
 * settings (by design decision). All components compute the same alpha from
 * the same subsystem query, so overlapping volumes blend toward the same
 * target and the overlap is harmless.
 *
 * Editor note: component tick does not run outside PIE, so BlendWeight
 * stays 0 and the volume reports itself disabled — the editor viewport is
 * never fogged.
 */
UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UUnderwaterPostProcessComponent
	: public USceneComponent
	, public IInterface_PostProcessVolume
{
	GENERATED_BODY()

public:
	UUnderwaterPostProcessComponent();

	// --- UActorComponent ---
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// --- IInterface_PostProcessVolume ---
	virtual bool EncompassesPoint(
		FVector Point, float SphereRadius = 0.0f,
		float* OutDistanceToPoint = nullptr) override;
	virtual FPostProcessVolumeProperties GetProperties() const override;
#if DEBUG_POST_PROCESS_VOLUME_ENABLE
	virtual FString GetDebugName() const override;
#endif

	// -------------------------------------------------------------------
	// Surface transition
	// -------------------------------------------------------------------

	/** Vertical fade band below the surface (cm). Camera this far under = full effect. */
	UPROPERTY(EditAnywhere, Category = "Underwater", meta = (ClampMin = "1.0"))
	float BlendDistance = 50.0f;

	/** Volume priority against other post-process volumes in the level. */
	UPROPERTY(EditAnywhere, Category = "Underwater")
	float Priority = 10.0f;

	// -------------------------------------------------------------------
	// Depth gradient
	// -------------------------------------------------------------------

	/**
	 * The depth gradient: grade stops from surface to deep, interpolated by
	 * camera depth. Keep sorted by Depth (the component re-sorts on edit
	 * and BeginPlay as a safety net). Defaults: barely-there at the
	 * surface -> light teal mid band -> dark blue at the deep end.
	 */
	UPROPERTY(EditAnywhere, Category = "Underwater|Depth Gradient", meta = (TitleProperty = "Depth"))
	TArray<FUnderwaterGradeStop> DepthGrade;

	// -------------------------------------------------------------------
	// Optional blendable material (swirl / fog / waterline mask)
	// -------------------------------------------------------------------

	/** Post-process domain material. Optional — the grade works without it. */
	UPROPERTY(EditAnywhere, Category = "Underwater|Material")
	TSoftObjectPtr<UMaterialInterface> UnderwaterMaterial;

	/** Exponential fog density pushed to the material. */
	UPROPERTY(EditAnywhere, Category = "Underwater|Material", meta = (ClampMin = "0.0"))
	float FogDensity = 0.02f;

	/** Fog / scatter colour pushed to the material. */
	UPROPERTY(EditAnywhere, Category = "Underwater|Material")
	FLinearColor WaterTint = FLinearColor(0.0f, 0.3f, 0.5f, 1.0f);

	// -------------------------------------------------------------------
	// Full settings block (advanced)
	// -------------------------------------------------------------------

	/**
	 * The complete post-process settings handed to the renderer. The depth
	 * gradient overwrites the exposure/gain/saturation/vignette fields each
	 * frame while submerged; every other field (depth of field, bloom, film
	 * grain, ...) can be overridden freely for extra underwater flavour.
	 */
	UPROPERTY(EditAnywhere, Category = "Underwater|Advanced", meta = (ShowOnlyInnerProperties))
	FPostProcessSettings Settings;

private:
	/** 0 = at/above surface, 1 = fully submerged. Polled by GetProperties(). */
	float BlendWeight = 0.0f;

	/** MID created from UnderwaterMaterial at BeginPlay. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BlendableMID;

	/** Interpolate the gradient at a camera depth (cm). Smoothstep between
		bracketing stops; clamps to first/last outside the authored range. */
	FUnderwaterGradeStop EvaluateDepthGrade(float CameraDepth) const;

	/** Write one (interpolated) stop into the Settings overrides. */
	void ApplyGradeSettings(const FUnderwaterGradeStop& Stop);

	/** Keep DepthGrade sorted by depth. */
	void SortDepthGrade();

	/** Push the static (non-per-frame) parameters to the MID. */
	void PushStaticMaterialParams();

	/** Apply a computed submersion alpha to weight and MID. */
	void SetSubmergedAlpha(float Alpha, float SurfaceZ, float CameraDepth);
};
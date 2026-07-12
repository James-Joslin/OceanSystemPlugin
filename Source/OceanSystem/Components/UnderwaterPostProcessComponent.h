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
 * Unbound post-process volume that activates when the player camera goes
 * below the water surface.
 *
 * Implementation note — why this is NOT a UPostProcessComponent subclass:
 * UPostProcessComponent is MinimalAPI in the engine, so its virtual
 * overrides are not exported and it cannot be subclassed from a plugin
 * module (LNK2001 on OnRegister/OnUnregister/Serialize/PostInitProperties).
 * Instead this component implements IInterface_PostProcessVolume directly —
 * the same interface the renderer polls for APostProcessVolume — and
 * registers itself in the world's post-process volume list. Identical
 * behaviour, no engine subclassing.
 *
 * Design:
 *   - Unbound: GetProperties() reports bIsUnbound, so no bounds need to
 *     track the wave surface.
 *   - Each tick queries WaveParameterSubsystem::GetFullWaveHeight() at the
 *     camera position (all visual layers — physics LOD would flicker near
 *     the surface by the amplitude of the excluded detail layers).
 *   - BlendWeight ramps 0 -> 1 over BlendDistance below the surface. The
 *     renderer polls it via GetProperties() every frame.
 *
 * Look (matches the prototype recipe):
 *   - Built-in FPostProcessSettings: exposure bias down, colour gain toward
 *     teal, saturation pull, vignette. No material required for these.
 *     The five grade properties below are copied into Settings and OWN
 *     those five fields; everything else in the exposed Settings struct
 *     (e.g. depth of field for underwater blur) is yours to edit freely.
 *   - Optional UnderwaterMaterial blendable for what built-ins can't do:
 *     swirl distortion, depth fog, per-pixel waterline masking. The MID
 *     receives EffectStrength / SurfaceZ / CameraDepth / FogDensity /
 *     WaterTint (see UnderwaterPP namespace in the .cpp).
 *
 * Multiple bodies: every water actor carries one of these with identical
 * settings (by design decision). All components compute the same alpha from
 * the same subsystem query, so overlapping volumes blend toward the same
 * target and the overlap is harmless. Per-body looks later = edit defaults.
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
	// Built-in grade (no material needed)
	// -------------------------------------------------------------------

	/** Exposure bias when fully submerged. Negative = darker. */
	UPROPERTY(EditAnywhere, Category = "Underwater|Grade")
	float ExposureBias = -1.0f;

	/** Colour gain when submerged — multiplies scene colour toward blue/teal. */
	UPROPERTY(EditAnywhere, Category = "Underwater|Grade")
	FLinearColor UnderwaterTint = FLinearColor(0.35f, 0.75f, 0.80f, 1.0f);

	/** Saturation when submerged. 1 = unchanged. */
	UPROPERTY(EditAnywhere, Category = "Underwater|Grade", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float Saturation = 0.65f;

	/** Vignette when submerged. */
	UPROPERTY(EditAnywhere, Category = "Underwater|Grade", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VignetteIntensity = 0.5f;

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
	 * The complete post-process settings handed to the renderer. The five
	 * grade properties above overwrite their corresponding fields here;
	 * every other field (depth of field, bloom, film grain, ...) can be
	 * overridden freely for extra underwater flavour without code changes.
	 */
	UPROPERTY(EditAnywhere, Category = "Underwater|Advanced", meta = (ShowOnlyInnerProperties))
	FPostProcessSettings Settings;

private:
	/** 0 = at/above surface, 1 = fully submerged. Polled by GetProperties(). */
	float BlendWeight = 0.0f;

	/** MID created from UnderwaterMaterial at BeginPlay. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BlendableMID;

	/** Copy the grade properties into the Settings overrides. */
	void ApplyGradeSettings();

	/** Push the static (non-per-frame) parameters to the MID. */
	void PushStaticMaterialParams();

	/** Apply a computed submersion alpha to weight and MID. */
	void SetSubmergedAlpha(float Alpha, float SurfaceZ, float CameraDepth);
};
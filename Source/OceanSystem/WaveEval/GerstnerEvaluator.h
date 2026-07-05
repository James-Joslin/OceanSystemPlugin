// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "../Types/OceanTypes.h"

class USplineComponent;

// ---------------------------------------------------------------------------
// FGerstnerResult
// ---------------------------------------------------------------------------

/**
 * Output from a CPU Gerstner wave evaluation.
 *
 * All fields are always populated. FoldIntensity is only physically
 * meaningful when computed with all visual layers (full eval), but is
 * computed in physics-LOD mode too since it's free.
 */
struct OCEANSYSTEM_API FGerstnerResult
{
	/** World-space displacement from the undisplaced position. */
	FVector Displacement = FVector::ZeroVector;

	/** Analytical surface normal (normalised). */
	FVector Normal = FVector::UpVector;

	/** Final world-space Z of the water surface (BaseZ + Displacement.Z). */
	float WorldZ = 0.0f;

	/** Jacobian fold intensity [0,1]. Indicates wave folding/breaking. */
	float FoldIntensity = 0.0f;
};

// ---------------------------------------------------------------------------
// FGerstnerEvaluator
// ---------------------------------------------------------------------------

/**
 * Static utility class for CPU-side Gerstner wave evaluation.
 *
 * All public methods delegate to a single EvaluateInternal() that mirrors
 * the HLSL in GerstnerWave.ush exactly. Any change to either must be
 * reflected in the other. Test parity at known inputs.
 *
 * Two evaluation tiers:
 *   Full     — all visual layers. Used by spray detection, underwater PP,
 *              debug visualisation.
 *   Physics  — first PhysicsLayerCount layers only. Used by buoyancy.
 *              Detail ripples are skipped — dominant swell drives physics.
 */
class OCEANSYSTEM_API FGerstnerEvaluator
{
public:
	// -------------------------------------------------------------------
	// Utilities — ported from GerstnerWave.ush for CPU parity
	// -------------------------------------------------------------------

	/** Domain warp — bends straight crests into organic curves.
		Apply to WorldPos before evaluation. Three octaves at irrational
		ratios to prevent visible periodicity. */
	static FVector DomainWarpPosition(
		const FVector& WorldPos, float Time,
		float WarpFrequency, float WarpAmount);

	/** Asymmetric crest shaping. Sharpness=1.0 is unchanged sine.
		>1 gives peaked crests with broad flat troughs.
		Output is zero-mean over a full cycle — required so that skipping
		layers on the physics path never shifts the resting water level. */
	static float SharpenSine(float SinValue, float Sharpness);

	/** Cycle-average of the uncorrected sharpened sine. Subtracted inside
		SharpenSine to keep each layer DC-free. Must be mirrored in
		GerstnerWave.ush (PARITY CONTRACT). */
	static float SharpenSineMean(float Sharpness);

	/** Derivative of SharpenSine for matching normals. */
	static float SharpenSineDerivative(float SinValue, float CosValue, float Sharpness);

	// -------------------------------------------------------------------
	// Flat-surface evaluators (ocean, lake)
	// -------------------------------------------------------------------

	/**
	 * Full evaluation — all visual layers.
	 * Use for spray detection, underwater PP threshold, debug vis.
	 */
	static FGerstnerResult Evaluate(
		const FVector& WorldPos, float Time,
		float BaseZ, const FWaveConfig& Config);

	/**
	 * Physics-LOD evaluation — first PhysicsLayerCount layers only.
	 * Use for buoyancy force calculation.
	 */
	static FGerstnerResult EvaluatePhysics(
		const FVector& WorldPos, float Time,
		float BaseZ, const FWaveConfig& Config);

	// -------------------------------------------------------------------
	// Visual evaluators — include domain warp + crest sharpening
	// -------------------------------------------------------------------

	/**
	 * Full visual evaluation with domain warp and crest sharpening.
	 * Matches the GPU EvaluateGerstnerDisplacementVisual output.
	 * Use for any CPU query that must match the rendered surface.
	 */
	static FGerstnerResult EvaluateVisual(
		const FVector& WorldPos, float Time,
		float BaseZ, const FWaveConfig& Config,
		float WarpFrequency, float WarpAmount, float CrestSharpness);

	/**
	 * Physics-LOD visual evaluation with domain warp and crest sharpening.
	 * Use for buoyancy so objects float on the visible surface.
	 */
	static FGerstnerResult EvaluatePhysicsVisual(
		const FVector& WorldPos, float Time,
		float BaseZ, const FWaveConfig& Config,
		float WarpFrequency, float WarpAmount, float CrestSharpness);

	// -------------------------------------------------------------------
	// Spline-surface evaluators (river)
	// -------------------------------------------------------------------

	/**
	 * Full evaluation along a spline.
	 * Finds closest point on spline, uses its Z as BaseZ.
	 * Use for spray detection on rivers.
	 */
	static FGerstnerResult EvaluateAlongSpline(
		const FVector& WorldPos, float Time,
		const USplineComponent* Spline, const FWaveConfig& Config);

	/**
	 * Physics-LOD evaluation along a spline.
	 * Use for buoyancy on rivers.
	 */
	static FGerstnerResult EvaluatePhysicsAlongSpline(
		const FVector& WorldPos, float Time,
		const USplineComponent* Spline, const FWaveConfig& Config);

	// -------------------------------------------------------------------
	// Blended evaluator (blend zones between bodies)
	// -------------------------------------------------------------------

	/**
	 * Physics-LOD blended evaluation between two water body configs.
	 * Evaluates both configs independently and lerps by Alpha.
	 * Alpha = 0 → pure A, Alpha = 1 → pure B.
	 */
	static FGerstnerResult EvaluatePhysicsBlended(
		const FVector& WorldPos, float Time,
		float BaseZA, const FWaveConfig& ConfigA,
		float BaseZB, const FWaveConfig& ConfigB,
		float Alpha);

private:
	/**
	 * Core Gerstner loop, parameterised by layer count.
	 * Mirrors GerstnerWave.ush EvaluateGerstnerFromTexture line-for-line.
	 * CrestSharpness = 1.0 gives the standard unmodified Gerstner.
	 */
	static FGerstnerResult EvaluateInternal(
		const FVector& WorldPos, float Time,
		float BaseZ, const FWaveConfig& Config,
		int32 LayerCount, float CrestSharpness = 1.0f);
};
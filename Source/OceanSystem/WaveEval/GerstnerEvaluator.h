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
	 *
	 * This is the SINGLE implementation of the Gerstner math on the CPU.
	 * It mirrors GerstnerWave.ush line-for-line. All public methods
	 * delegate here with the appropriate LayerCount:
	 *   Full:    Config.GetVisualLayerCount()
	 *   Physics: Config.GetPhysicsLayerCount()
	 *
	 * @param WorldPos   Undisplaced world position (Z-up).
	 * @param Time       Raw world time in seconds (TimeScale applied internally).
	 * @param BaseZ      Resting water surface height.
	 * @param Config     Wave configuration (layers must already be sorted).
	 * @param LayerCount Number of layers to evaluate.
	 */
	static FGerstnerResult EvaluateInternal(
		const FVector& WorldPos, float Time,
		float BaseZ, const FWaveConfig& Config,
		int32 LayerCount);
};
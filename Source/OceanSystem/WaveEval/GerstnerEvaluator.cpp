// Copyright James Joslin. All Rights Reserved.

#include "GerstnerEvaluator.h"
#include "Components/SplineComponent.h"

// ---------------------------------------------------------------------------
// Public — Flat-surface evaluators
// ---------------------------------------------------------------------------

FGerstnerResult FGerstnerEvaluator::Evaluate(
	const FVector& WorldPos, float Time,
	float BaseZ, const FWaveConfig& Config)
{
	return EvaluateInternal(WorldPos, Time, BaseZ, Config,
		Config.GetVisualLayerCount());
}

FGerstnerResult FGerstnerEvaluator::EvaluatePhysics(
	const FVector& WorldPos, float Time,
	float BaseZ, const FWaveConfig& Config)
{
	return EvaluateInternal(WorldPos, Time, BaseZ, Config,
		Config.GetPhysicsLayerCount());
}

// ---------------------------------------------------------------------------
// Public — Spline-surface evaluators
// ---------------------------------------------------------------------------

FGerstnerResult FGerstnerEvaluator::EvaluateAlongSpline(
	const FVector& WorldPos, float Time,
	const USplineComponent* Spline, const FWaveConfig& Config)
{
	if (!Spline)
	{
		// Fallback: treat as flat surface at WorldPos.Z
		return Evaluate(WorldPos, Time, WorldPos.Z, Config);
	}

	const FVector ClosestPoint = Spline->FindLocationClosestToWorldLocation(
		WorldPos, ESplineCoordinateSpace::World);
	const float SplineBaseZ = ClosestPoint.Z;

	return EvaluateInternal(WorldPos, Time, SplineBaseZ, Config,
		Config.GetVisualLayerCount());
}

FGerstnerResult FGerstnerEvaluator::EvaluatePhysicsAlongSpline(
	const FVector& WorldPos, float Time,
	const USplineComponent* Spline, const FWaveConfig& Config)
{
	if (!Spline)
	{
		return EvaluatePhysics(WorldPos, Time, WorldPos.Z, Config);
	}

	const FVector ClosestPoint = Spline->FindLocationClosestToWorldLocation(
		WorldPos, ESplineCoordinateSpace::World);
	const float SplineBaseZ = ClosestPoint.Z;

	return EvaluateInternal(WorldPos, Time, SplineBaseZ, Config,
		Config.GetPhysicsLayerCount());
}

// ---------------------------------------------------------------------------
// Public — Blended evaluator
// ---------------------------------------------------------------------------

FGerstnerResult FGerstnerEvaluator::EvaluatePhysicsBlended(
	const FVector& WorldPos, float Time,
	float BaseZA, const FWaveConfig& ConfigA,
	float BaseZB, const FWaveConfig& ConfigB,
	float Alpha)
{
	const FGerstnerResult ResultA = EvaluateInternal(
		WorldPos, Time, BaseZA, ConfigA, ConfigA.GetPhysicsLayerCount());
	const FGerstnerResult ResultB = EvaluateInternal(
		WorldPos, Time, BaseZB, ConfigB, ConfigB.GetPhysicsLayerCount());

	const float ClampedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);

	FGerstnerResult Blended;
	Blended.Displacement = FMath::Lerp(ResultA.Displacement, ResultB.Displacement, ClampedAlpha);
	Blended.Normal = FMath::Lerp(ResultA.Normal, ResultB.Normal, ClampedAlpha).GetSafeNormal();
	Blended.WorldZ = FMath::Lerp(ResultA.WorldZ, ResultB.WorldZ, ClampedAlpha);
	Blended.FoldIntensity = FMath::Lerp(ResultA.FoldIntensity, ResultB.FoldIntensity, ClampedAlpha);

	return Blended;
}

// ---------------------------------------------------------------------------
// Private — Core Gerstner loop
// ---------------------------------------------------------------------------
//
// PARITY CONTRACT: This loop mirrors GerstnerWave.ush EvaluateGerstner()
// line-for-line. The same wave number, phase angle, displacement,
// normal accumulation, and Jacobian sum. Any change here must be
// reflected in the .ush and vice versa.
//
// Notation (matching .ush):
//   k     = 2π / Wavelength          (wave number)
//   Q     = Steepness                (0–1)
//   A     = Amplitude                (metres)
//   theta = k * dot(Dir, P.xy) - k * Speed * Time + PhaseOffset
//
//   Displacement.XY += Q * A * Dir * cos(theta)
//   Displacement.Z  += A * sin(theta)
//
//   Normal.XY -= Dir * k * A * cos(theta)
//   Normal.Z  -= Q * k * A * sin(theta)      (starts at 1.0)
//
//   JacobianSum += Q * k * A * cos(theta)
//   FoldIntensity = clamp(JacobianSum, 0, 1)
// ---------------------------------------------------------------------------

FGerstnerResult FGerstnerEvaluator::EvaluateInternal(
	const FVector& WorldPos, float Time,
	float BaseZ, const FWaveConfig& Config,
	int32 LayerCount)
{
	FGerstnerResult Result;
	Result.Displacement = FVector::ZeroVector;

	// Normal accumulation — un-normalised, Z starts at 1.0
	float NormX = 0.0f;
	float NormY = 0.0f;
	float NormZ = 1.0f;

	// Jacobian sum for fold detection
	float JacobianSum = 0.0f;

	// Apply per-body time scale
	const float ScaledTime = Time * Config.TimeScale;

	// Clamp layer count to what's actually available
	const int32 Count = FMath::Clamp(LayerCount, 0, Config.Layers.Num());

	for (int32 i = 0; i < Count; ++i)
	{
		const FGerstnerWaveLayer& Layer = Config.Layers[i];

		// Wave number
		const float k = UE_TWO_PI / Layer.Wavelength;

		// 2D dot product: Dir · WorldPos.XY
		const float DotXY = Layer.Direction.X * WorldPos.X
			+ Layer.Direction.Y * WorldPos.Y;

		// Phase angle (mirrors .ush exactly)
		const float Theta = k * DotXY - k * Layer.Speed * ScaledTime + Layer.PhaseOffset;

		float SinTheta, CosTheta;
		FMath::SinCos(&SinTheta, &CosTheta, Theta);

		const float Q = Layer.Steepness;
		const float A = Layer.Amplitude;
		const float kA = k * A;

		// ----- Displacement -----
		Result.Displacement.X += Q * A * Layer.Direction.X * CosTheta;
		Result.Displacement.Y += Q * A * Layer.Direction.Y * CosTheta;
		Result.Displacement.Z += A * SinTheta;

		// ----- Normal accumulation -----
		NormX -= Layer.Direction.X * kA * CosTheta;
		NormY -= Layer.Direction.Y * kA * CosTheta;
		NormZ -= Q * kA * SinTheta;

		// ----- Fold detection (Jacobian) -----
		JacobianSum += Q * kA * CosTheta;
	}

	// Normalise accumulated normal
	const FVector RawNormal(NormX, NormY, NormZ);
	Result.Normal = RawNormal.GetSafeNormal();

	// Final world Z
	Result.WorldZ = BaseZ + Result.Displacement.Z;

	// Fold intensity — saturate to [0,1]
	Result.FoldIntensity = FMath::Clamp(JacobianSum, 0.0f, 1.0f);

	return Result;
}
// Copyright James Joslin. All Rights Reserved.

#include "GerstnerEvaluator.h"
#include "Components/SplineComponent.h"

// ---------------------------------------------------------------------------
// Utilities — ported from GerstnerWave.ush
// ---------------------------------------------------------------------------

FVector FGerstnerEvaluator::DomainWarpPosition(
	const FVector& WorldPos, float Time,
	float WarpFrequency, float WarpAmount)
{
	const float WPX = WorldPos.X * WarpFrequency;
	const float WPY = WorldPos.Y * WarpFrequency;

	const float WX = FMath::Sin(WPY * 1.0f + Time * 0.05f) * WarpAmount
		+ FMath::Sin(WPY * 2.3f + WPX * 0.7f + Time * 0.03f) * WarpAmount * 0.5f
		+ FMath::Sin(WPY * 4.1f + WPX * 1.3f + Time * 0.07f) * WarpAmount * 0.2f;

	const float WY = FMath::Cos(WPX * 1.0f + Time * 0.04f) * WarpAmount
		+ FMath::Cos(WPX * 1.9f + WPY * 0.6f + Time * 0.06f) * WarpAmount * 0.5f
		+ FMath::Cos(WPX * 3.7f + WPY * 1.1f + Time * 0.05f) * WarpAmount * 0.2f;

	return FVector(WorldPos.X + WX, WorldPos.Y + WY, WorldPos.Z);
}

float FGerstnerEvaluator::SharpenSineMean(float Sharpness)
{
	// Mean of the *uncorrected* sharpened sine (2*((1+sin)/2)^p - 1) over a
	// full wave cycle. Exact value is 2*Gamma(p+0.5)/(sqrt(pi)*Gamma(p+1)) - 1;
	// the closed form below approximates it to within ~0.3% for p in [1, 4]
	// and contains only ops available in HLSL, so the .ush can mirror it
	// exactly (PARITY CONTRACT).
	return 2.0f / FMath::Sqrt(UE_PI * (Sharpness + 0.267f)) - 1.0f;
}

float FGerstnerEvaluator::SharpenSine(float SinValue, float Sharpness)
{
	const float Mapped = SinValue * 0.5f + 0.5f;
	const float Sharp = FMath::Pow(Mapped, Sharpness);

	// Re-centre so the sharpened wave is ZERO-MEAN over a cycle.
	//
	// pow() on the remapped sine narrows the crests and widens the troughs,
	// which drags the cycle average below zero (-0.151 * Amplitude at p=1.5).
	// Every sharpened layer therefore contributed a hidden negative DC offset
	// proportional to its amplitude. Because the physics path evaluates only
	// PhysicsLayerCount layers while the GPU displaces with ALL layers, the
	// visible surface carried a much larger negative DC than the CPU query —
	// so buoyancy queries reported a water level that was permanently ABOVE
	// the rendered surface by 0.151 * (sum of excluded layer amplitudes).
	// Subtracting the mean makes each layer DC-free, so skipping layers only
	// removes oscillation, never shifts the resting water level.
	return (Sharp * 2.0f - 1.0f) - SharpenSineMean(Sharpness);
}

float FGerstnerEvaluator::SharpenSineDerivative(float SinValue, float CosValue, float Sharpness)
{
	// Unchanged by the zero-mean correction in SharpenSine — the subtracted
	// mean is constant per layer, so its derivative is zero.
	const float Mapped = SinValue * 0.5f + 0.5f;
	return Sharpness * FMath::Pow(FMath::Max(Mapped, 0.001f), Sharpness - 1.0f) * CosValue;
}

// ---------------------------------------------------------------------------
// Public — Flat-surface evaluators
// ---------------------------------------------------------------------------

FGerstnerResult FGerstnerEvaluator::Evaluate(
	const FVector& WorldPos, float Time,
	float BaseZ, const FWaveConfig& Config)
{
	return EvaluateInternal(WorldPos, Time, BaseZ, Config,
		Config.GetTotalLayerCount());
}

FGerstnerResult FGerstnerEvaluator::EvaluatePhysics(
	const FVector& WorldPos, float Time,
	float BaseZ, const FWaveConfig& Config)
{
	return EvaluateInternal(WorldPos, Time, BaseZ, Config,
		Config.GetPhysicsLayerCount());
}

// ---------------------------------------------------------------------------
// Public — Visual evaluators (domain warp + crest sharpening)
// ---------------------------------------------------------------------------

FGerstnerResult FGerstnerEvaluator::EvaluateVisual(
	const FVector& WorldPos, float Time,
	float BaseZ, const FWaveConfig& Config,
	float WarpFrequency, float WarpAmount, float CrestSharpness)
{
	const FVector WarpedPos = (WarpAmount > UE_KINDA_SMALL_NUMBER)
		? DomainWarpPosition(WorldPos, Time * Config.TimeScale, WarpFrequency, WarpAmount)
		: WorldPos;

	return EvaluateInternal(WarpedPos, Time, BaseZ, Config,
		Config.GetTotalLayerCount(), CrestSharpness);
}

FGerstnerResult FGerstnerEvaluator::EvaluatePhysicsVisual(
	const FVector& WorldPos, float Time,
	float BaseZ, const FWaveConfig& Config,
	float WarpFrequency, float WarpAmount, float CrestSharpness)
{
	const FVector WarpedPos = (WarpAmount > UE_KINDA_SMALL_NUMBER)
		? DomainWarpPosition(WorldPos, Time * Config.TimeScale, WarpFrequency, WarpAmount)
		: WorldPos;

	return EvaluateInternal(WarpedPos, Time, BaseZ, Config,
		Config.GetPhysicsLayerCount(), CrestSharpness);
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
		Config.GetTotalLayerCount());
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
	int32 LayerCount, float CrestSharpness)
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

	// Whether to apply crest sharpening
	const bool bSharpen = !FMath::IsNearlyEqual(CrestSharpness, 1.0f, 0.01f);

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

		if (bSharpen)
		{
			Result.Displacement.Z += A * SharpenSine(SinTheta, CrestSharpness);
		}
		else
		{
			Result.Displacement.Z += A * SinTheta;
		}

		// ----- Normal accumulation -----
		NormX -= Layer.Direction.X * kA * CosTheta;
		NormY -= Layer.Direction.Y * kA * CosTheta;

		if (bSharpen)
		{
			NormZ -= Q * kA * SharpenSineDerivative(SinTheta, CosTheta, CrestSharpness);
		}
		else
		{
			NormZ -= Q * kA * SinTheta;
		}

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
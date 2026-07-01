// Copyright James Joslin. All Rights Reserved.

#include "WaveGenerator.h"
#include "Math/RandomStream.h"

// ===================================================================
// fBm and Domain Warp � private to this translation unit
// ===================================================================
//
// Fractional Brownian motion (fBm) stacks multiple octaves of Perlin
// noise at increasing frequency and decreasing amplitude, producing
// natural-looking variation with both broad structure and fine detail.
//
// Domain warping distorts the noise input using a second noise
// evaluation, creating organic swirl patterns rather than uniform
// variation. The result is that nearby waves in the spectrum don't
// just get randomly different values � they get coherently different
// values, as if shaped by an underlying physical process.
// ===================================================================

namespace
{
	/**
	 * 1D fractional Brownian motion using Perlin noise.
	 *
	 * @param X           Input coordinate.
	 * @param Octaves     Number of noise layers (1-8).
	 * @param Lacunarity  Frequency multiplier per octave (~2.0).
	 * @param Gain        Amplitude multiplier per octave (~0.5).
	 * @return Normalised noise value in approximately [-1, 1].
	 */
	float FBm1D(float X, int32 Octaves, float Lacunarity, float Gain)
	{
		float Value = 0.0f;
		float Amplitude = 1.0f;
		float Frequency = 1.0f;
		float MaxValue = 0.0f;

		for (int32 i = 0; i < Octaves; ++i)
		{
			Value += Amplitude * FMath::PerlinNoise1D(X * Frequency);
			MaxValue += Amplitude;
			Frequency *= Lacunarity;
			Amplitude *= Gain;
		}

		return (MaxValue > UE_KINDA_SMALL_NUMBER) ? (Value / MaxValue) : 0.0f;
	}

	/**
	 * Domain-warped 1D fBm.
	 *
	 * Evaluates fBm at an input coordinate that has itself been displaced
	 * by a secondary fBm evaluation. The offset (5.2) decorrelates the
	 * warp noise from the primary noise.
	 *
	 * @param WarpStrength  How far the input is displaced (0 = no warp).
	 */
	float WarpedFBm1D(float X, int32 Octaves, float Lacunarity, float Gain, float WarpStrength)
	{
		if (WarpStrength < UE_KINDA_SMALL_NUMBER)
		{
			return FBm1D(X, Octaves, Lacunarity, Gain);
		}

		const float Warp = FBm1D(X + 5.2f, Octaves, Lacunarity, Gain);
		return FBm1D(X + Warp * WarpStrength, Octaves, Lacunarity, Gain);
	}
}

// ===================================================================
// FWaveGeneratorConfig::Generate
// ===================================================================
//
// For each wave i in [0, NumWaves):
//
//   1. t = i / (N-1) � normalised spectrum position (0=largest, 1=smallest).
//
//   2. Base values from power-curve interpolation:
//        Wavelength = Lerp(Max, Min, pow(t, Falloff))
//        Amplitude  = Lerp(Max, Min, pow(t, Falloff))
//        Steepness  = Lerp(LargeWave, SmallWave, pow(t, Falloff))
//
//   3. Domain-warped fBm noise applied multiplicatively to amplitude
//      and wavelength. Sampled at positions derived from wave index
//      and seed � nearby spectrum waves get correlated variation,
//      domain warp creates organic clustering.
//
//   4. Simple randomness (FRandomStream) for per-parameter jitter.
//
//   5. Direction from golden-angle distribution within angular spread.
//
//   6. Speed from deep-water dispersion relation.
//
//   7. Layers sorted by amplitude descending after generation.
// ===================================================================

FWaveConfig FWaveGeneratorConfig::Generate() const
{
	FRandomStream RNG(Seed);

	FWaveConfig Config;
	Config.PhysicsLayerCount = PhysicsLayerCount;
	Config.TimeScale = TimeScale;

	const int32 Count = FMath::Clamp(NumWaves, 1, FWaveConfig::MaxLayers);

	static constexpr float GoldenAngleDeg = 137.508f;
	static constexpr float Gravity = 981.0f;	// cm/s² (UE world units)

	// Noise sampling base � offset by seed for unique patterns per body.
	const float NoiseBase = static_cast<float>(Seed) * 0.317f;

	for (int32 i = 0; i < Count; ++i)
	{
		const float T = (Count > 1)
			? static_cast<float>(i) / static_cast<float>(Count - 1)
			: 0.0f;

		// ---- Base values from falloff curves ----
		float WL = FMath::Lerp(MaxWavelength, MinWavelength,
			FMath::Pow(T, WavelengthFalloff));
		float Amp = FMath::Lerp(MaxAmplitude, MinAmplitude,
			FMath::Pow(T, AmplitudeFalloff));
		float Steep = FMath::Lerp(LargeWaveSteepness, SmallWaveSteepness,
			FMath::Pow(T, SteepnessFalloff));

		// ---- Domain-warped fBm noise ----
		if (NoiseStrength > UE_KINDA_SMALL_NUMBER)
		{
			// Sample at the spectrum position, scaled so the noise has
			// visible variation across the wave range. Amplitude and
			// wavelength get decorrelated channels (offset by 100).
			const float NoiseInputAmp = NoiseBase + T * 4.0f;
			const float NoiseInputWL = NoiseBase + T * 4.0f + 100.0f;

			const float AmpNoise = WarpedFBm1D(
				NoiseInputAmp, NoiseOctaves, NoiseLacunarity, NoiseGain, NoiseWarpStrength);
			const float WLNoise = WarpedFBm1D(
				NoiseInputWL, NoiseOctaves, NoiseLacunarity, NoiseGain, NoiseWarpStrength);

			Amp *= 1.0f + AmpNoise * NoiseStrength;
			WL *= 1.0f + WLNoise * NoiseStrength;
		}

		// ---- Simple randomness perturbation ----
		if (Randomness > UE_KINDA_SMALL_NUMBER)
		{
			WL *= (1.0f + RNG.FRandRange(-1.0f, 1.0f) * Randomness * 0.4f);
			Amp *= (1.0f + RNG.FRandRange(-1.0f, 1.0f) * Randomness * 0.4f);
			Steep += RNG.FRandRange(-1.0f, 1.0f) * Randomness * 0.15f;
		}

		// Clamp to safe ranges
		WL = FMath::Max(WL, 0.1f);
		Amp = FMath::Max(Amp, 0.001f);
		Steep = FMath::Clamp(Steep, 0.0f, 10.0f);

		// ---- Direction ----
		float RawOffsetDeg = FMath::Fmod(static_cast<float>(i) * GoldenAngleDeg, 360.0f);
		if (RawOffsetDeg > 180.0f)
		{
			RawOffsetDeg -= 360.0f;
		}

		const float SpreadFactor = FMath::Clamp(DirectionAngularSpread / 360.0f, 0.0f, 1.0f);
		float AngleDeg = DominantWindAngle + RawOffsetDeg * SpreadFactor;

		if (Randomness > UE_KINDA_SMALL_NUMBER)
		{
			AngleDeg += RNG.FRandRange(-1.0f, 1.0f) * Randomness * 25.0f;
		}

		const float AngleRad = FMath::DegreesToRadians(AngleDeg);
		FVector2D Dir(FMath::Cos(AngleRad), FMath::Sin(AngleRad));
		Dir.Normalize();

		// ---- Speed (dispersion relation) ----
		const float BaseSpeed = FMath::Sqrt(Gravity * WL / UE_TWO_PI);
		float Speed = BaseSpeed * GlobalSpeedMultiplier;

		if (Randomness > UE_KINDA_SMALL_NUMBER)
		{
			Speed *= (1.0f + RNG.FRandRange(-1.0f, 1.0f) * Randomness * 0.2f);
		}
		Speed = FMath::Max(Speed, 0.01f);

		// ---- Phase offset ----
		const float PhaseOffset = RNG.FRandRange(0.0f, UE_TWO_PI);

		// ---- Build layer ----
		FGerstnerWaveLayer Layer;
		Layer.Direction = Dir;
		Layer.Amplitude = Amp;
		Layer.Wavelength = WL;
		Layer.Steepness = Steep;
		Layer.Speed = Speed;
		Layer.PhaseOffset = PhaseOffset;

		Config.Layers.Add(Layer);
	}

	Config.SortLayers();
	Config.bDirty = true;

	return Config;
}
// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "../Types/OceanTypes.h"
#include "WaveGenerator.generated.h"

/**
 * High-level wave spectrum generator with domain-warped fBm noise.
 *
 * Instead of authoring every layer by hand, set min/max ranges for
 * wavelength, amplitude, and steepness, then call Generate() to produce
 * a complete FWaveConfig. The generator distributes N layers between the
 * extremes using power-curve falloffs, applies domain-warped fBm noise
 * for organic variation, derives speed from the deep-water dispersion
 * relation, and spreads directions around a dominant wind angle using
 * golden-angle distribution.
 *
 * Up to 16 layers can be generated. All are sent to the GPU via the
 * data texture MID and are available for CPU evaluation (buoyancy, spray).
 *
 * Workflow:
 *   1. Tune the generator properties in the Details panel.
 *   2. Click "Generate Waves" on the OceanBodyComponent.
 *   3. The WaveConfig.Layers array populates.
 *   4. Hand-tweak individual layers if needed.
 */
USTRUCT(BlueprintType)
struct OCEANSYSTEM_API FWaveGeneratorConfig
{
	GENERATED_BODY()

	// -------------------------------------------------------------------
	// Wave Count
	// -------------------------------------------------------------------

	/**
	 * Number of wave layers to generate (1-128).
	 * The first 16 (MaxVisualLayers) are pushed to the GPU material.
	 * All layers are available for CPU evaluation (buoyancy, spray).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator",
		meta = (ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "16"))
	int32 NumWaves = 8;

	/** Deterministic seed for randomisation. Same seed = same waves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator")
	int32 Seed = 0;

	/**
	 * Simple perturbation strength (0-1).
	 * 0 = perfectly regular stepped values between min and max.
	 * 1 = maximum random variation around the stepped values.
	 * Applied independently per parameter � uncorrelated noise.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator",
		meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float Randomness = 0.2f;

	// -------------------------------------------------------------------
	// Wavelength
	// -------------------------------------------------------------------

	/** Shortest wavelength in cm (smallest detail ripple). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Wavelength",
		meta = (ClampMin = "10.0", UIMin = "10.0", UIMax = "1000.0"))
	float MinWavelength = 500.0f;

	/** Longest wavelength in cm (dominant swell). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Wavelength",
		meta = (ClampMin = "100.0", UIMin = "100.0", UIMax = "5000.0"))
	float MaxWavelength = 12000.0f;

	/**
	 * Power curve for wavelength distribution.
	 * 1.0 = linear spacing from max to min.
	 * >1  = more layers clustered toward the large end.
	 * <1  = more layers clustered toward the small end.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Wavelength",
		meta = (ClampMin = "0.1", ClampMax = "5.0", UIMin = "0.5", UIMax = "3.0"))
	float WavelengthFalloff = 2.0f;

	// -------------------------------------------------------------------
	// Amplitude
	// -------------------------------------------------------------------

	/** Smallest amplitude in cm (detail ripple height). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Amplitude",
		meta = (ClampMin = "0.1", UIMin = "1.0", UIMax = "100.0"))
	float MinAmplitude = 5.0f;

	/** Largest amplitude in cm (dominant swell height). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Amplitude",
		meta = (ClampMin = "1.0", UIMin = "1.0", UIMax = "2000.0"))
	float MaxAmplitude = 30.0f;

	/**
	 * Power curve for amplitude distribution.
	 * 1.0 = linear falloff from max to min.
	 * >1  = amplitude drops off quickly � fewer large waves.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Amplitude",
		meta = (ClampMin = "0.1", ClampMax = "5.0", UIMin = "0.5", UIMax = "3.0"))
	float AmplitudeFalloff = 2.0f;

	// -------------------------------------------------------------------
	// Steepness
	// -------------------------------------------------------------------

	/**
	 * Steepness for the largest waves. Low values (0.05-0.15) give broad
	 * rolling swells. Above 0.8, waves fold and drive the foam mask.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Steepness",
		meta = (ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.01", UIMax = "3"))
	float LargeWaveSteepness = 0.08f;

	/** Steepness for the smallest waves. Higher values (0.5-0.8) give sharp peaked chop. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Steepness",
		meta = (ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.01", UIMax = "3.0"))
	float SmallWaveSteepness = 0.65f;

	/**
	 * Power curve for steepness interpolation.
	 * 1.0 = linear transition from large to small steepness.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Steepness",
		meta = (ClampMin = "0.1", ClampMax = "5.0", UIMin = "0.1", UIMax = "5.0"))
	float SteepnessFalloff = 1.0f;

	// -------------------------------------------------------------------
	// Direction
	// -------------------------------------------------------------------

	/** Primary wind direction in degrees (0 = +X, 90 = +Y). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Direction",
		meta = (UIMin = "0.0", UIMax = "360.0"))
	float DominantWindAngle = 0.0f;

	/**
	 * Angular spread of wave directions in degrees.
	 * 0    = all waves travel in the dominant direction.
	 * 180  = waves spread across a half-circle.
	 * 360  = waves spread across the full circle.
	 * Uses golden-angle distribution for even coverage.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Direction",
		meta = (ClampMin = "0.0", ClampMax = "360.0", UIMin = "0.0", UIMax = "360.0"))
	float DirectionAngularSpread = 120.0f;

	// -------------------------------------------------------------------
	// Speed
	// -------------------------------------------------------------------

	/**
	 * Global speed multiplier applied after deriving phase velocity from
	 * wavelength via the deep-water dispersion relation:
	 *   BaseSpeed = sqrt(g x Wavelength / 2pi)
	 * Set to 1.0 for physically correct speeds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Speed",
		meta = (ClampMin = "0.01", UIMin = "0.01", UIMax = "3.0"))
	float GlobalSpeedMultiplier = 1.0f;

	// -------------------------------------------------------------------
	// Domain-Warped fBm Noise
	// -------------------------------------------------------------------

	/**
	 * Strength of the fBm noise applied to amplitude and wavelength.
	 * 0 = no noise (only the base falloff curves + simple randomness).
	 * Higher values add increasingly dramatic organic variation.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Noise",
		meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0"))
	float NoiseStrength = 0.3f;

	/** Number of fBm octaves. More octaves = finer noise detail. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Noise",
		meta = (ClampMin = "1", ClampMax = "8", UIMin = "1", UIMax = "6"))
	int32 NoiseOctaves = 3;

	/** Frequency multiplier per octave (typically ~2.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Noise",
		meta = (ClampMin = "1.0", ClampMax = "4.0", UIMin = "1.5", UIMax = "3.0"))
	float NoiseLacunarity = 2.0f;

	/**
	 * Amplitude multiplier per octave (persistence, typically ~0.5).
	 * Lower = smoother, higher = more detail.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Noise",
		meta = (ClampMin = "0.1", ClampMax = "0.9", UIMin = "0.3", UIMax = "0.7"))
	float NoiseGain = 0.5f;

	/**
	 * Domain warp intensity. Distorts the noise input coordinates using
	 * a second noise evaluation, creating organic swirl patterns in the
	 * wave spectrum rather than uniform variation.
	 * 0 = no warp (plain fBm). Higher = more distortion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Noise",
		meta = (ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.5"))
	float NoiseWarpStrength = 0.5f;

	// -------------------------------------------------------------------
	// Physics
	// -------------------------------------------------------------------

	/** PhysicsLayerCount on the generated FWaveConfig. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Physics",
		meta = (ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "6"))
	int32 PhysicsLayerCount = 3;

	/** TimeScale on the generated FWaveConfig. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generator|Physics",
		meta = (ClampMin = "0.0", UIMin = "0.1", UIMax = "3.0"))
	float TimeScale = 1.0f;

	// -------------------------------------------------------------------
	// Generation
	// -------------------------------------------------------------------

	/**
	 * Generate a complete FWaveConfig from these parameters.
	 * Deterministic � same inputs always produce the same output.
	 */
	FWaveConfig Generate() const;
};
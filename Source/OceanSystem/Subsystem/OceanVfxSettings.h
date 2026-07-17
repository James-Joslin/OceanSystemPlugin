// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "OceanVfxSettings.generated.h"

class UOceanVfxVariantSet;

/**
 * Project-wide ocean VFX configuration.
 * Project Settings -> Plugins -> Ocean VFX.
 *
 * Subsystems (rock spray now, vessel/crest later) read their variant set
 * assignments and tuning from here, so designers assign assets once per
 * project rather than per level or per instance.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Ocean VFX"))
class OCEANSYSTEM_API UOceanVfxSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	// -------------------------------------------------------------------
	// Rock impact spray (D2)
	// -------------------------------------------------------------------

	/**
	 * Variant set fired when a swell breaks against a rock spray point.
	 * Author systems as FINITE (burst mode). Unset = rock spray disabled.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Rock Spray")
	TSoftObjectPtr<UOceanVfxVariantSet> RockImpactVariantSet;

	/**
	 * Static mesh socket name prefix marking spray points. Socket X-axis
	 * = outward face normal. Optional convention: socket relative scale X
	 * = intensity multiplier for hero crags.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Rock Spray")
	FString SpraySocketPrefix = TEXT("Spray_");

	/**
	 * Scan-time pre-filter: keep only points within this vertical distance
	 * (cm) of the water surface sampled at scan time. Should comfortably
	 * exceed max wave amplitude. Kills cliff-top sockets PCG placed high.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Rock Spray",
		meta = (ClampMin = "100.0"))
	float VerticalScanMargin = 800.0f;

	/**
	 * Only points within this camera distance (cm) are evaluated per tick.
	 * Keep at or below the VFX subsystem's MaxEffectDistance — evaluating
	 * points whose bursts would be range-rejected is wasted work.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Rock Spray",
		meta = (ClampMin = "1000.0"))
	float ActiveRadius = 12000.0f;

	/**
	 * Minimum upward surface crossing speed (cm/s) to fire at all —
	 * ripples below this fizzle silently.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Rock Spray",
		meta = (ClampMin = "0.0"))
	float MinCrossingSpeed = 60.0f;

	/**
	 * Crossing speed (cm/s) mapped to Intensity = 1. Speeds between Min
	 * and this lerp intensity; big swells detonate, small ones spritz.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Rock Spray",
		meta = (ClampMin = "1.0"))
	float FullIntensitySpeed = 400.0f;
};
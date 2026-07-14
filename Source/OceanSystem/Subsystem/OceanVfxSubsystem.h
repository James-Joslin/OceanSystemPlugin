// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/DataAsset.h"
#include "OceanVfxSubsystem.generated.h"

class UNiagaraSystem;
class UNiagaraComponent;

// ===================================================================
// UOceanVfxVariantSet
// ===================================================================

/**
 * An authored set of interchangeable Niagara systems for one effect
 * category (e.g. "rock impact spray", "bow spray", "whitecap burst").
 *
 * Variation model:
 *   - Bursts re-roll a random system every fire (avoiding the last pick
 *     so the same asset never fires twice running).
 *   - Sustained effects pick once at activation and hold their variant.
 *   - Continuous variation within one system comes from the user
 *     parameter contract (see UOceanVfxSubsystem).
 *
 * Niagara authoring contract:
 *   - Burst sets: systems MUST be finite (one loop / fixed burst) or they
 *     never finish and drain the engine's component pool.
 *   - Sustained sets: systems loop, and should map spawn rate / size to
 *     User.Intensity so heartbeat intensity visibly modulates them.
 *   - User parameters (declare even if a given system ignores some):
 *       Intensity        (float,  0-1)
 *       ImpactDirection  (vector, unit)
 *       WaterVelocity    (vector, cm/s)
 */
UCLASS(BlueprintType)
class OCEANSYSTEM_API UOceanVfxVariantSet : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Interchangeable Niagara systems for this effect category. */
	UPROPERTY(EditAnywhere, Category = "Vfx")
	TArray<TObjectPtr<UNiagaraSystem>> Systems;

	/**
	 * Burst mode only: minimum seconds between fires at (approximately)
	 * the same location. Enforced by the subsystem via a spatial hash so
	 * a churning sea can't machine-gun one rock.
	 */
	UPROPERTY(EditAnywhere, Category = "Vfx", meta = (ClampMin = "0.0"))
	float MinRefireSeconds = 0.75f;

	/**
	 * Camera-distance cull override in cm. 0 = use the subsystem default.
	 * Set higher for hero effects (big bow plumes), lower for cheap
	 * ambient pops.
	 */
	UPROPERTY(EditAnywhere, Category = "Vfx", meta = (ClampMin = "0.0"))
	float MaxDistanceOverride = 0.0f;
};

// ===================================================================
// FOceanVfxHandle
// ===================================================================

/**
 * Opaque handle identifying one sustained (looping) effect. Obtained from
 * the first UpdateSustained() call; pass it back on every heartbeat.
 * Default-constructed = invalid = "give me a new effect".
 */
USTRUCT(BlueprintType)
struct FOceanVfxHandle
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Id = INDEX_NONE;

	bool IsValid() const { return Id != INDEX_NONE; }
	void Reset() { Id = INDEX_NONE; }
};

// ===================================================================
// UOceanVfxSubsystem
// ===================================================================

/**
 * Central pooled Niagara service for all water VFX. Nothing in the plugin
 * spawns Niagara directly — rocks, vessels, and ambient crest spray all
 * request effects here.
 *
 * Two lifecycles:
 *
 *   BURSTS (fire-and-forget): RequestBurst(). Spawned through the engine's
 *   Niagara component pool with AutoRelease — the pool reclaims the
 *   component when the (finite) system finishes. Per-site refire cooldowns
 *   and a per-frame cap live here.
 *
 *   SUSTAINED (looping) — heartbeat pattern: call UpdateSustained() EVERY
 *   TICK the effect should stay alive; the call doubles as the parameter
 *   feed (position follows the bow, intensity follows speed). Anything not
 *   refreshed for SustainedGraceSeconds is auto-released — a destroyed or
 *   sunk boat's spray dies on its own; nothing can loop orphaned at sea.
 *   Release uses Deactivate-then-pool so existing particles live out their
 *   lifetime and the effect tapers naturally.
 *
 * Culling is central and owned here: far-away bursts are withheld, and
 * sustained effects have their components released beyond range (with
 * hysteresis) while their logical entries persist — the effect pops back
 * in when the camera returns, requesters stay oblivious.
 *
 * Debug console commands (see .cpp):
 *   Ocean.Vfx.Burst <VariantSetAssetPath>
 *   Ocean.Vfx.Sustained <VariantSetAssetPath> [Seconds]
 *   Ocean.Vfx.Stats
 */
UCLASS()
class OCEANSYSTEM_API UOceanVfxSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// --- UTickableWorldSubsystem ---
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual void Deinitialize() override;

	// -------------------------------------------------------------------
	// Burst API (fire-and-forget)
	// -------------------------------------------------------------------

	/**
	 * Fire a one-shot effect. Returns false if rejected (cooldown, range,
	 * per-frame cap, or no valid systems) — callers can treat rejection as
	 * "not this time" and simply try again on their next trigger.
	 *
	 * @param Direction      Unit vector the effect should face (e.g. rock
	 *                       face normal, slam up-vector). Zero = up.
	 * @param Intensity      0-1, pushed as User.Intensity.
	 * @param WaterVelocity  Wave velocity at the site (cm/s), pushed as
	 *                       User.WaterVelocity.
	 */
	bool RequestBurst(
		UOceanVfxVariantSet* VariantSet,
		const FVector& Location,
		const FVector& Direction,
		float Intensity,
		const FVector& WaterVelocity = FVector::ZeroVector);

	// -------------------------------------------------------------------
	// Sustained API (looping, heartbeat)
	// -------------------------------------------------------------------

	/**
	 * Assert a sustained effect for this tick. Pass an invalid handle the
	 * first time; store and pass back the returned handle every tick after.
	 * Not calling this for SustainedGraceSeconds releases the effect.
	 *
	 * The variant is chosen once at first activation and held for the
	 * effect's lifetime.
	 */
	FOceanVfxHandle UpdateSustained(
		FOceanVfxHandle Handle,
		UOceanVfxVariantSet* VariantSet,
		const FVector& Location,
		const FVector& Direction,
		float Intensity,
		const FVector& WaterVelocity = FVector::ZeroVector);

	/**
	 * Optional explicit early release (the heartbeat timeout makes this
	 * unnecessary in normal flow — use for immediate teardown on e.g.
	 * effect handoff). Existing particles still live out their lifetime.
	 */
	void ReleaseSustained(FOceanVfxHandle& Handle);

	// -------------------------------------------------------------------
	// Tuning (adjust from code/Blueprint before or during play)
	// -------------------------------------------------------------------

	/** Default camera-distance cull for effects (cm). ~150 m. */
	float MaxEffectDistance = 15000.0f;

	/** Extra distance beyond the cull range before an active sustained
		component is released — prevents flicker at the boundary. */
	float CullHysteresis = 2000.0f;

	/** Seconds a sustained effect survives without a heartbeat. ~2-4
		frames at 30-60 fps; generous enough for a hitch, tight enough
		that orphans die invisibly fast. */
	float SustainedGraceSeconds = 0.15f;

	/** Hard cap on bursts started in a single frame. */
	int32 MaxBurstsPerFrame = 8;

private:
	/** One live (or range-culled) sustained effect. */
	struct FSustainedEffect
	{
		TWeakObjectPtr<UOceanVfxVariantSet> VariantSet;

		/** Variant chosen at first activation; held for the lifetime. */
		TWeakObjectPtr<UNiagaraSystem> ChosenSystem;

		/** Live pooled component; null while range-culled. */
		TWeakObjectPtr<UNiagaraComponent> Component;

		double LastHeartbeatTime = 0.0;
		FVector LastLocation = FVector::ZeroVector;
	};

	TMap<int32, FSustainedEffect> SustainedEffects;
	int32 NextHandleId = 1;

	/** Spatial-hash burst cooldowns: quantised location -> last fire time. */
	TMap<uint64, double> BurstCooldowns;
	double LastCooldownPurgeTime = 0.0;

	/** Last variant index per set, for burst last-pick avoidance. */
	TMap<TWeakObjectPtr<const UOceanVfxVariantSet>, int32> LastVariantPick;

	int32 BurstsThisFrame = 0;

	// Debug console command state (self-heartbeats a sustained effect).
	TWeakObjectPtr<UOceanVfxVariantSet> DebugSustainedSet;
	FOceanVfxHandle DebugSustainedHandle;
	double DebugSustainedUntil = 0.0;
	FVector DebugSustainedLocation = FVector::ZeroVector;

	UNiagaraSystem* PickVariant(const UOceanVfxVariantSet& Set, bool bAvoidLastPick);
	UNiagaraComponent* SpawnPooled(
		UNiagaraSystem& System, const FVector& Location,
		const FVector& Direction, bool bAutoRelease);
	void PushUserParams(
		UNiagaraComponent& Component, const FVector& Direction,
		float Intensity, const FVector& WaterVelocity) const;
	void ReleaseEffectComponent(FSustainedEffect& Effect) const;
	bool GetViewLocation(FVector& OutLocation) const;
	float ResolveMaxDistance(const UOceanVfxVariantSet& Set) const;
	static uint64 MakeSpatialKey(const FVector& Location);

	friend class FOceanVfxDebugCommands;
};
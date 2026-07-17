// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "../Subsystem/OceanVfxSubsystem.h" // FOceanVfxHandle
#include "OceanVesselEffectsComponent.generated.h"

class UOceanBuoyancyComponent;
class UOceanVfxVariantSet;
class UWaveParameterSubsystem;
class UPrimitiveComponent;

/**
 * Vessel water VFX (Phase D3). Add alongside an OceanBuoyancyComponent —
 * this component performs (almost) no wave queries of its own; it reads
 * the per-point sample state the buoyancy pass already computed each tick
 * and turns it into effect requests on UOceanVfxSubsystem.
 *
 * Effects driven (each optional — unset variant set = feature off):
 *
 *   BOW SPRAY (sustained): active while the bow rides the waterline with
 *   forward speed relative to the water (boat velocity minus wave
 *   velocity — the port that started Phase D). Follows the bow, faces
 *   forward, intensity scales with relative speed. Bow priority falls
 *   out naturally: the bow has the highest relative speed into oncoming
 *   water.
 *
 *   SIDE CONTACT SPRAY (sustained): the same rule at gentler thresholds
 *   on the widest port/starboard hull points.
 *
 *   HULL SLAM (burst): any hull point crossing above->below water with
 *   real downward speed — the boat coming down off a swell. Intensity
 *   from impact speed.
 *
 *   WAKE (sustained): particle wake positioned at the visual water
 *   surface behind the stern, trailing while under way. This is the v1
 *   particle wake — a displacement/render-target wake is a deferred
 *   aesthetics project.
 *
 * Bow / stern / port / starboard are classified automatically from the
 * buoyancy sample layout in local space (max X / min X / min Y / max Y)
 * — no authoring beyond the buoyancy points the vessel already has.
 *
 * All sustained effects run on the subsystem's heartbeat: if this
 * component (or the whole boat) is destroyed mid-spray, everything
 * tapers out on its own.
 */
 /** Which local axis the vessel's bow points along. */
UENUM(BlueprintType)
enum class EOceanVesselForwardAxis : uint8
{
	PlusX  UMETA(DisplayName = "+X"),
	MinusX UMETA(DisplayName = "-X"),
	PlusY  UMETA(DisplayName = "+Y"),
	MinusY UMETA(DisplayName = "-Y")
};

UCLASS(ClassGroup = (OceanSystem), meta = (BlueprintSpawnableComponent))
class OCEANSYSTEM_API UOceanVesselEffectsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UOceanVesselEffectsComponent();

	/**
	 * The local axis the bow points along. Everything derives from this:
	 * bow/stern/port/starboard classification of the buoyancy samples,
	 * the spray/wake facing directions, and relative-speed measurement.
	 * Boats modelled to the UE convention keep +X; a hull that sails
	 * along its local Y sets +Y (or -Y), no mesh changes needed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx")
	EOceanVesselForwardAxis ForwardAxis = EOceanVesselForwardAxis::PlusX;

	// -------------------------------------------------------------------
	// Variant sets (unset = that effect disabled)
	// -------------------------------------------------------------------

	/** Sustained bow spray (looping systems). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx")
	TObjectPtr<UOceanVfxVariantSet> BowSpraySet;

	/** Sustained side-hull contact spray (looping systems). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx")
	TObjectPtr<UOceanVfxVariantSet> SideContactSet;

	/** Burst slam splash on hull re-entry (FINITE systems). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx")
	TObjectPtr<UOceanVfxVariantSet> SlamSet;

	/** Sustained stern wake (looping systems). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx")
	TObjectPtr<UOceanVfxVariantSet> WakeSet;

	/**
	 * Burst fired when an authored hull spray socket enters the water
	 * (FINITE systems). Sockets are authored on the hull's static mesh
	 * with the same convention as rocks: name starts with the project
	 * SpraySocketPrefix ("Spray_"), X-axis points out of the hull,
	 * relative scale X = intensity multiplier. Unlike rock sockets these
	 * are resolved through the live component transform every tick, so
	 * they ride the hull. Unset = feature off.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx")
	TObjectPtr<UOceanVfxVariantSet> HullSpraySet;

	// -------------------------------------------------------------------
	// Tuning
	// -------------------------------------------------------------------

	/** Relative forward speed (cm/s, vs the water) where bow spray starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "0.0"))
	float MinSpraySpeed = 150.0f;

	/** Relative forward speed mapped to Intensity = 1 for spray and wake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "1.0"))
	float FullIntensitySpeed = 800.0f;

	/**
	 * Waterline band (cm): the bow/side point counts as "in contact"
	 * while the surface is within this distance of it (above or below).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "1.0"))
	float WaterlineBand = 60.0f;

	/** Minimum downward point speed (cm/s) for a re-entry slam burst. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "0.0"))
	float SlamMinImpactSpeed = 250.0f;

	/** Downward speed mapped to slam Intensity = 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "1.0"))
	float SlamFullIntensitySpeed = 900.0f;

	/** Minimum water-entry closure speed (cm/s) for a hull socket burst —
		how fast the surface and the socket are approaching each other
		(covers both the hull dropping and a wave rising). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "0.0"))
	float HullSprayMinSpeed = 100.0f;

	/** Closure speed mapped to hull-socket Intensity = 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "1.0"))
	float HullSprayFullSpeed = 500.0f;

	/** Boat speed (cm/s) below which the wake shuts off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "0.0"))
	float WakeMinSpeed = 100.0f;

	/** Trailing pad (cm) added behind the stern to the wake spawn
		extent (User.Extent = hull length + this). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "0.0"))
	float WakeSternOffset = 200.0f;

	/** Side contact uses MinSpraySpeed x this (gentler trigger). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel Vfx",
		meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float SideContactSpeedFactor = 0.6f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UOceanBuoyancyComponent> Buoyancy;

	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> PhysicsBody;

	UPROPERTY(Transient)
	TObjectPtr<UWaveParameterSubsystem> WaveSubsystem;

	UPROPERTY(Transient)
	TObjectPtr<UOceanVfxSubsystem> VfxSubsystem;

	// Sample indices classified from the buoyancy layout at BeginPlay.
	int32 BowIndex = INDEX_NONE;
	int32 SternIndex = INDEX_NONE;
	int32 PortIndex = INDEX_NONE;
	int32 StarboardIndex = INDEX_NONE;

	// Sustained effect handles (heartbeat-managed by the subsystem).
	FOceanVfxHandle BowHandle;
	FOceanVfxHandle PortHandle;
	FOceanVfxHandle StarboardHandle;
	FOceanVfxHandle WakeHandle;

	/** Previous-frame submerged flags, index-aligned with samples. */
	TArray<bool> PrevSubmerged;

	/** One authored hull spray socket (rides the mesh component). */
	struct FHullSprayPoint
	{
		FName SocketName;
		float IntensityMul = 1.0f;
		float PrevDepth = 0.0f;
		bool bHavePrev = false;
	};
	TArray<FHullSprayPoint> HullSprayPoints;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> HullMesh;

	void GatherHullSpraySockets();
	void TickHullSpraySockets(float DeltaTime);

	void ClassifySamplePoints();
	void ReleaseAllHandles();

	/** Local-space forward unit vector for ForwardAxis. */
	FVector GetLocalForward() const;

	/** Sustained contact-spray rule shared by bow and sides. */
	void UpdateContactSpray(
		FOceanVfxHandle& Handle, UOceanVfxVariantSet* Set,
		const struct FOceanBuoyancySample& Sample, const FVector& Direction,
		float RelForwardSpeed, float SpeedThreshold, float IntensityScale);
};
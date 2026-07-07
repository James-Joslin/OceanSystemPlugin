// Copyright James Joslin. All Rights Reserved.

#include "RiverWaterBodyActor.h"
#include "../Components/OceanBodyComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

ARiverWaterBodyActor::ARiverWaterBodyActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// --- Spline root (river centreline) ---
	RiverSpline = CreateDefaultSubobject<USplineComponent>(TEXT("RiverSpline"));
	RootComponent = RiverSpline;

	// Give the default spline a reasonable starting shape — a gentle curve
	// rather than a straight line, so the river looks natural on placement.
	RiverSpline->SetSplinePoints(
		{
			FVector(0.0f, 0.0f, 0.0f),
			FVector(1500.0f, 500.0f, -20.0f),
			FVector(3000.0f, 0.0f, -50.0f)
		},
		ESplineCoordinateSpace::Local, /*bUpdateSpline=*/true);

	// --- OceanBody (river type, higher priority than ocean/lake) ---
	OceanBody = CreateDefaultSubobject<UOceanBodyComponent>(TEXT("OceanBody"));
	OceanBody->SetupAttachment(RiverSpline);
	OceanBody->BodyType = EOceanBodyType::River;
	OceanBody->Priority = 20;

	// Rivers don't use tiled mesh, so disable auto-sizing
	OceanBody->bAutoSizeExtentFromMesh = false;

	// Visual shaping — rivers use subtler values than open ocean
	OceanBody->DomainWarpFrequency = 0.0005f;
	OceanBody->DomainWarpAmount = 150.0f;
	OceanBody->CrestSharpness = 1.2f;

	// --- Wave generator defaults for a river ---
	// Small, fast, narrow-spread waves aligned to flow direction.
	// All spatial values in cm (UE world units).
	FWaveGeneratorConfig& Gen = OceanBody->WaveGenerator;
	Gen.NumWaves = 4;
	Gen.Seed = 200;
	Gen.Randomness = 0.2f;
	Gen.MinWavelength = 30.0f;      // 30cm — fine surface ripples
	Gen.MaxWavelength = 300.0f;     // 3m — gentle surface undulation
	Gen.WavelengthFalloff = 1.5f;
	Gen.MinAmplitude = 1.0f;        // 1cm
	Gen.MaxAmplitude = 5.0f;        // 5cm — subtle, not ocean-scale
	Gen.AmplitudeFalloff = 1.5f;
	Gen.LargeWaveSteepness = 0.15f;
	Gen.SmallWaveSteepness = 0.4f;
	Gen.SteepnessFalloff = 1.0f;
	Gen.DominantWindAngle = 0.0f;   // 0° = +X, aligned to default spline tangent
	Gen.DirectionAngularSpread = 40.0f;  // Narrow spread — waves follow the river
	Gen.GlobalSpeedMultiplier = 1.5f;    // Faster-moving water feel
	Gen.NoiseStrength = 0.15f;
	Gen.NoiseOctaves = 2;
	Gen.NoiseLacunarity = 2.0f;
	Gen.NoiseGain = 0.5f;
	Gen.NoiseWarpStrength = 0.3f;
	Gen.PhysicsLayerCount = 2;
	Gen.TimeScale = 1.0f;

	OceanBody->WaveConfig = Gen.Generate();

	// --- Detail wave generator for per-pixel river chop ---
	FWaveGeneratorConfig& Detail = OceanBody->DetailWaveGenerator;
	Detail.NumWaves = 4;
	Detail.Seed = 300;
	Detail.Randomness = 0.2f;
	Detail.MinWavelength = 10.0f;    // 10cm — very fine capillary ripples
	Detail.MaxWavelength = 100.0f;   // 1m
	Detail.WavelengthFalloff = 1.2f;
	Detail.MinAmplitude = 0.3f;
	Detail.MaxAmplitude = 2.0f;
	Detail.AmplitudeFalloff = 1.2f;
	Detail.LargeWaveSteepness = 0.3f;
	Detail.SmallWaveSteepness = 0.6f;
	Detail.SteepnessFalloff = 1.0f;
	Detail.DominantWindAngle = 0.0f;
	Detail.DirectionAngularSpread = 60.0f;
	Detail.GlobalSpeedMultiplier = 0.8f;
	Detail.NoiseStrength = 0.2f;
	Detail.NoiseOctaves = 2;
	Detail.NoiseLacunarity = 2.0f;
	Detail.NoiseGain = 0.5f;
	Detail.NoiseWarpStrength = 0.2f;
	Detail.PhysicsLayerCount = 0;  // never on CPU
	Detail.TimeScale = 1.0f;

	OceanBody->DetailWaveConfig = Detail.Generate();
}

// ===================================================================
// Lifecycle
// ===================================================================

void ARiverWaterBodyActor::BeginPlay()
{
	Super::BeginPlay();
	RebuildRiverMesh();
	RefreshMeshMaterial();
}

#if WITH_EDITOR
void ARiverWaterBodyActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!OceanBody || !RiverSpline)
	{
		return;
	}

	// Ensure MID exists and body is registered with subsystem.
	OceanBody->InitializeWaterBody();

	// Rebuild spline meshes if none exist yet (first construction).
	if (SegmentMeshes.Num() == 0)
	{
		RebuildRiverMesh();
	}

	RefreshMeshMaterial();
}

void ARiverWaterBodyActor::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName MemberName = PropertyChangedEvent.GetMemberPropertyName();

	// Rebuild mesh when geometry properties change
	if (MemberName == GET_MEMBER_NAME_CHECKED(ARiverWaterBodyActor, RiverWidth)
		|| MemberName == GET_MEMBER_NAME_CHECKED(ARiverWaterBodyActor, SegmentSubdivisions)
		|| MemberName == GET_MEMBER_NAME_CHECKED(ARiverWaterBodyActor, RiverSpline))
	{
		RebuildRiverMesh();
		RefreshMeshMaterial();
	}

	// FlowSpeed change — re-register to push new value to subsystem/MID
	if (MemberName == GET_MEMBER_NAME_CHECKED(ARiverWaterBodyActor, FlowSpeed))
	{
		if (OceanBody)
		{
			OceanBody->InitializeWaterBody();
		}
	}
}
#endif

// ===================================================================
// Mesh Generation
// ===================================================================

void ARiverWaterBodyActor::RebuildRiverMesh()
{
	DestroySegmentMeshes();

	if (!RiverSpline)
	{
		return;
	}

	const int32 NumPoints = RiverSpline->GetNumberOfSplinePoints();
	if (NumPoints < 2)
	{
		return;
	}

	const int32 NumSegments = RiverSpline->IsClosedLoop()
		? NumPoints
		: NumPoints - 1;

	SegmentMeshes.Reserve(NumSegments);

	for (int32 i = 0; i < NumSegments; ++i)
	{
		const int32 NextIdx = (i + 1) % NumPoints;

		const FVector StartPos = RiverSpline->GetLocationAtSplinePoint(
			i, ESplineCoordinateSpace::Local);
		const FVector StartTangent = RiverSpline->GetTangentAtSplinePoint(
			i, ESplineCoordinateSpace::Local);

		const FVector EndPos = RiverSpline->GetLocationAtSplinePoint(
			NextIdx, ESplineCoordinateSpace::Local);
		const FVector EndTangent = RiverSpline->GetTangentAtSplinePoint(
			NextIdx, ESplineCoordinateSpace::Local);

		USplineMeshComponent* SegmentMesh = CreateSegmentMesh(
			i, StartPos, StartTangent, EndPos, EndTangent);

		if (SegmentMesh)
		{
			SegmentMeshes.Add(SegmentMesh);
		}
	}

	// Update the OceanBody's registry entry so the subsystem knows
	// this river's spline and half-width for spatial queries.
	if (OceanBody)
	{
		OceanBody->InitializeWaterBody();
	}
}

USplineMeshComponent* ARiverWaterBodyActor::CreateSegmentMesh(
	int32 SegmentIndex,
	const FVector& StartPos, const FVector& StartTangent,
	const FVector& EndPos, const FVector& EndTangent)
{
	const FName MeshName = *FString::Printf(TEXT("RiverSegment_%d"), SegmentIndex);

	USplineMeshComponent* Mesh = NewObject<USplineMeshComponent>(
		this, MeshName);

	if (!Mesh)
	{
		return nullptr;
	}

	Mesh->SetupAttachment(RiverSpline);
	Mesh->RegisterComponent();

	// USplineMeshComponent needs a static mesh source to deform.
	// Use a runtime-generated flat plane. The SplineMeshComponent
	// will deform it along the spline. We use the Engine's built-in
	// plane mesh as the source geometry.
	//
	// The flat plane is oriented so that:
	//   X axis = along the spline (forward/flow direction)
	//   Y axis = cross-section (river width)
	//   Z axis = up (displaced by Gerstner WPO in the material)
	//
	// The SplineMeshComponent handles the deformation from the flat
	// plane into the spline curve automatically.

	// Find the engine's 1x1 plane static mesh
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));

	if (PlaneMesh)
	{
		Mesh->SetStaticMesh(PlaneMesh);
	}

	// Scale to river width — the engine plane is 100x100 units,
	// so we scale Y to match RiverWidth.
	const float HalfWidth = RiverWidth * 0.5f;
	Mesh->SetRelativeScale3D(FVector(1.0f, RiverWidth / 100.0f, 1.0f));

	// Set spline points for this segment
	Mesh->SetStartAndEnd(
		StartPos, StartTangent,
		EndPos, EndTangent,
		/*bUpdateMesh=*/true);

	// Forward axis is X — the spline mesh deforms along X
	Mesh->SetForwardAxis(ESplineMeshAxis::X);

	// Disable collision — water surfaces don't block movement,
	// buoyancy is handled by the CPU evaluator.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Mobility must match the actor for spline mesh to work
	Mesh->SetMobility(EComponentMobility::Movable);

	return Mesh;
}

void ARiverWaterBodyActor::DestroySegmentMeshes()
{
	for (USplineMeshComponent* Mesh : SegmentMeshes)
	{
		if (Mesh)
		{
			Mesh->DestroyComponent();
		}
	}
	SegmentMeshes.Empty();
}

// ===================================================================
// Material
// ===================================================================

void ARiverWaterBodyActor::RefreshMeshMaterial()
{
	if (!OceanBody)
	{
		return;
	}

	UMaterialInstanceDynamic* MID = OceanBody->GetMaterialInstance();
	UMaterialInterface* FallbackMat = OceanBody->BaseMaterial.Get();

	UMaterialInterface* MatToApply = MID
		? static_cast<UMaterialInterface*>(MID)
		: FallbackMat;

	if (!MatToApply)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RiverWaterBodyActor '%s': No material — set BaseMaterial on OceanBody."),
			*GetName());
		return;
	}

	for (USplineMeshComponent* Mesh : SegmentMeshes)
	{
		if (Mesh)
		{
			Mesh->SetMaterial(0, MatToApply);
		}
	}
}
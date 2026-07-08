// Copyright James Joslin. All Rights Reserved.

#include "RiverWaterBodyActor.h"
#include "../Components/OceanBodyComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"

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
	Gen.DominantWindAngle = 0.0f;   // 0 degrees = +X, aligned to default spline tangent
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

	// Work out how many segments the current spline needs.
	const int32 NumPoints = RiverSpline->GetNumberOfSplinePoints();
	const int32 ExpectedSegments = (NumPoints < 2) ? 0
		: (RiverSpline->IsClosedLoop() ? NumPoints : NumPoints - 1);

	if (SegmentMeshes.Num() == ExpectedSegments && ExpectedSegments > 0)
	{
		// Same segment count — update transforms in place.
		// This is the hot path during spline point drags: no component
		// creation/destruction, just SetStartAndEnd on existing meshes.
		UpdateSegmentTransforms();
	}
	else
	{
		// Segment count changed (point added/removed, or first build).
		RebuildRiverMesh();
	}

	RefreshMeshMaterial();
}

void ARiverWaterBodyActor::PostEditChangeProperty(
	FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName MemberName = PropertyChangedEvent.GetMemberPropertyName();

	// Source mesh changed — full rebuild to pick up the new asset
	if (MemberName == GET_MEMBER_NAME_CHECKED(ARiverWaterBodyActor, RiverSegmentMesh))
	{
		RebuildRiverMesh();
		RefreshMeshMaterial();
	}

	// Width changed — update scale on existing segments (no rebuild)
	if (MemberName == GET_MEMBER_NAME_CHECKED(ARiverWaterBodyActor, RiverWidth))
	{
		UpdateSegmentTransforms();
		// Re-register so subsystem picks up new half-width for spatial queries
		if (OceanBody)
		{
			OceanBody->InitializeWaterBody();
		}
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
// Source Mesh
// ===================================================================

UStaticMesh* ARiverWaterBodyActor::GetSegmentMesh() const
{
	// Try the user-assigned mesh first
	UStaticMesh* Mesh = RiverSegmentMesh.LoadSynchronous();
	if (Mesh)
	{
		return Mesh;
	}

	// Fallback: engine's basic plane (2 tris, no LODs).
	// Usable for testing but not production — log a reminder.
	UStaticMesh* FallbackMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));

	if (FallbackMesh)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RiverWaterBodyActor '%s': No RiverSegmentMesh assigned — "
				"using engine Plane fallback (2 tris, no LODs). Assign a "
				"subdivided plane mesh for proper WPO and LOD support."),
			*GetName());
	}

	return FallbackMesh;
}

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

	// Mobility must be set before registration so the render proxy
	// is created with the right flags.
	Mesh->SetMobility(EComponentMobility::Movable);

	// Disable collision — water surfaces don't block movement,
	// buoyancy is handled by the CPU evaluator.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Assign the source mesh — user asset with LODs, or engine fallback.
	UStaticMesh* SourceMesh = GetSegmentMesh();
	if (SourceMesh)
	{
		Mesh->SetStaticMesh(SourceMesh);
	}

	// Forward axis MUST be set before SetStartAndEnd — it controls
	// which axis of the source mesh is remapped along the spline.
	// X = the plane's length axis follows the river path.
	Mesh->SetForwardAxis(ESplineMeshAxis::X);

	// Cross-section width via SplineMeshComponent's own scale system.
	// The source mesh should be 100 units wide in Y (standard convention).
	// StartScale/EndScale control the two axes perpendicular to
	// ForwardAxis (Y and Z when ForwardAxis=X).
	const float WidthScale = RiverWidth / 100.0f;
	Mesh->SetStartScale(FVector2D(WidthScale, 1.0f));
	Mesh->SetEndScale(FVector2D(WidthScale, 1.0f));

	// Set spline points for this segment — drives the deformation.
	Mesh->SetStartAndEnd(
		StartPos, StartTangent,
		EndPos, EndTangent,
		/*bUpdateMesh=*/true);

	Mesh->RegisterComponent();

	return Mesh;
}

void ARiverWaterBodyActor::UpdateSegmentTransforms()
{
	if (!RiverSpline)
	{
		return;
	}

	const int32 NumPoints = RiverSpline->GetNumberOfSplinePoints();
	const int32 NumSegments = SegmentMeshes.Num();
	const float WidthScale = RiverWidth / 100.0f;

	for (int32 i = 0; i < NumSegments; ++i)
	{
		USplineMeshComponent* Mesh = SegmentMeshes[i];
		if (!Mesh)
		{
			continue;
		}

		const int32 NextIdx = (i + 1) % NumPoints;

		const FVector StartPos = RiverSpline->GetLocationAtSplinePoint(
			i, ESplineCoordinateSpace::Local);
		const FVector StartTangent = RiverSpline->GetTangentAtSplinePoint(
			i, ESplineCoordinateSpace::Local);
		const FVector EndPos = RiverSpline->GetLocationAtSplinePoint(
			NextIdx, ESplineCoordinateSpace::Local);
		const FVector EndTangent = RiverSpline->GetTangentAtSplinePoint(
			NextIdx, ESplineCoordinateSpace::Local);

		Mesh->SetStartAndEnd(
			StartPos, StartTangent,
			EndPos, EndTangent,
			/*bUpdateMesh=*/true);

		Mesh->SetStartScale(FVector2D(WidthScale, 1.0f));
		Mesh->SetEndScale(FVector2D(WidthScale, 1.0f));
	}
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
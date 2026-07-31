// Copyright James Joslin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "../Types/OceanTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWaterConnectionSmootherStepTest,
	"OceanSystem.WaterConnection.SmootherStepEndpoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaterConnectionSmootherStepTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Alpha below the source boundary is zero"),
		WaterConnectionMath::SmootherStep01(-1.0f), 0.0f);
	TestEqual(TEXT("Alpha at the source boundary is zero"),
		WaterConnectionMath::SmootherStep01(0.0f), 0.0f);
	TestEqual(TEXT("Alpha at the target boundary is one"),
		WaterConnectionMath::SmootherStep01(1.0f), 1.0f);
	TestEqual(TEXT("Alpha beyond the target boundary is one"),
		WaterConnectionMath::SmootherStep01(2.0f), 1.0f);
	TestTrue(TEXT("The midpoint is symmetric"),
		FMath::IsNearlyEqual(
			WaterConnectionMath::SmootherStep01(0.5f), 0.5f, 1.e-6f));

	const float Epsilon = 1.e-3f;
	const float SourceSlope =
		WaterConnectionMath::SmootherStep01(Epsilon) / Epsilon;
	const float TargetSlope =
		(1.0f - WaterConnectionMath::SmootherStep01(1.0f - Epsilon)) / Epsilon;
	// This mirrors shader float precision; cancellation near alpha=1 makes
	// the target-side finite difference noisier than the analytic derivative.
	TestTrue(TEXT("Source derivative tends to zero"), SourceSlope < 1.e-3f);
	TestTrue(TEXT("Target derivative tends to zero"), TargetSlope < 1.e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWaterConnectionAbsoluteHeightTest,
	"OceanSystem.WaterConnection.AbsoluteHeightBlend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaterConnectionAbsoluteHeightTest::RunTest(const FString& Parameters)
{
	constexpr float SourceWorldZ = 125.0f;
	constexpr float TargetWorldZ = 925.0f;
	TestEqual(TEXT("Source endpoint reproduces the source world height"),
		WaterConnectionMath::BlendAbsoluteHeight(
			SourceWorldZ, TargetWorldZ, 0.0f),
		SourceWorldZ);
	TestEqual(TEXT("Target endpoint reproduces the target world height"),
		WaterConnectionMath::BlendAbsoluteHeight(
			SourceWorldZ, TargetWorldZ, 1.0f),
		TargetWorldZ);
	TestEqual(TEXT("The blend includes the base-height difference"),
		WaterConnectionMath::BlendAbsoluteHeight(
			SourceWorldZ, TargetWorldZ, 0.25f),
		325.0f);
	return true;
}

#endif

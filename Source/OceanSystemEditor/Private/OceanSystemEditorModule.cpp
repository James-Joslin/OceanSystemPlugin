// Copyright James Joslin. All Rights Reserved.

#include "OceanSystemEditorModule.h"
#include "OceanBuoyancyComponentVisualizer.h"

// NOTE: adjust this include to match the runtime module's folder layout,
// e.g. "Components/OceanBuoyancyComponent.h".
#include "Components/OceanBuoyancyComponent.h"

#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Misc/CoreDelegates.h"

IMPLEMENT_MODULE(FOceanSystemEditorModule, OceanSystemEditor)

void FOceanSystemEditorModule::StartupModule()
{
	// GUnrealEd may not exist yet at module load — defer if needed.
	if (GUnrealEd)
	{
		RegisterVisualizers();
	}
	else
	{
		FCoreDelegates::OnPostEngineInit.AddRaw(
			this, &FOceanSystemEditorModule::RegisterVisualizers);
	}
}

void FOceanSystemEditorModule::RegisterVisualizers()
{
	if (!GUnrealEd || bVisualizersRegistered)
	{
		return;
	}

	TSharedPtr<FComponentVisualizer> BuoyancyVisualizer =
		MakeShared<FOceanBuoyancyComponentVisualizer>();

	GUnrealEd->RegisterComponentVisualizer(
		UOceanBuoyancyComponent::StaticClass()->GetFName(), BuoyancyVisualizer);
	BuoyancyVisualizer->OnRegister();

	bVisualizersRegistered = true;
}

void FOceanSystemEditorModule::ShutdownModule()
{
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);

	if (GUnrealEd && bVisualizersRegistered)
	{
		GUnrealEd->UnregisterComponentVisualizer(
			UOceanBuoyancyComponent::StaticClass()->GetFName());
	}
}

// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Editor-only module for the Ocean System plugin.
 *
 * Registers component visualizers (currently the buoyancy sample point
 * visualizer). Never loaded in packaged builds.
 */
class FOceanSystemEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Registers visualizers with GUnrealEd (deferred until engine init). */
	void RegisterVisualizers();

	bool bVisualizersRegistered = false;
};

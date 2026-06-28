// Copyright James Joslin. All Rights Reserved.

#include "OceanSystemModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FOceanSystemModule"

void FOceanSystemModule::StartupModule()
{
	const FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("OceanSystemPlugin"))->GetBaseDir(),
		TEXT("Shaders")
	);

	AddShaderSourceDirectoryMapping(TEXT("/OceanSystem"), PluginShaderDir);
}

void FOceanSystemModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOceanSystemModule, OceanSystem)
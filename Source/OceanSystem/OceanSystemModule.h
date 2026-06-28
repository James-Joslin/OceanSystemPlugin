// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FOceanSystemModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
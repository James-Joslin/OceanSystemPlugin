// Copyright James Joslin. All Rights Reserved.

using UnrealBuildTool;

public class OceanSystemEditor : ModuleRules
{
    public OceanSystemEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "OceanSystem"        // the runtime module — component types live there
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UnrealEd",          // FComponentVisualizer, GUnrealEd, GEditor
            "EditorFramework",
            "Slate",
            "SlateCore",
            "InputCore"
        });
    }
}

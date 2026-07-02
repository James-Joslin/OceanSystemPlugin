using UnrealBuildTool;

public class OceanSystem : ModuleRules
{
    public OceanSystem(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "ProceduralMeshComponent",
            "Niagara"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "RenderCore",
            "RHI",
            "Projects"       // IPluginManager for shader source mapping
		});

        // Editor-only: viewport camera access for in-editor LOD
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
        }
    }
}
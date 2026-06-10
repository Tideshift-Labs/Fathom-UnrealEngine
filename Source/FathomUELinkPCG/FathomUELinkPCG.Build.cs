using UnrealBuildTool;

public class FathomUELinkPCG : ModuleRules
{
	public FathomUELinkPCG(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"FathomUELink",
			"AssetRegistry",
			"PCG",
		});
	}
}

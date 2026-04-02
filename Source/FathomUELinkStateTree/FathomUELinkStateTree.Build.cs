using UnrealBuildTool;

public class FathomUELinkStateTree : ModuleRules
{
	public FathomUELinkStateTree(ReadOnlyTargetRules Target) : base(Target)
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
			"PropertyBindingUtils",
			"StateTreeModule",
			"StateTreeEditorModule",
		});
	}
}

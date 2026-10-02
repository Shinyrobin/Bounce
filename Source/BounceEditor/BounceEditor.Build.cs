using UnrealBuildTool;

public class BounceEditor : ModuleRules
{
	public BounceEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"HTTPServer",
			"Json",
			"Sockets",
			"Bounce"
		});
	}
}

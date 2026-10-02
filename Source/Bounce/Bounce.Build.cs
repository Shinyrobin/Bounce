using UnrealBuildTool;

public class Bounce : ModuleRules
{
	public Bounce(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"PhysicsCore"
		});

		// BounceBot (scripted playtester)
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ImageCore",
			"Json"
		});
	}
}

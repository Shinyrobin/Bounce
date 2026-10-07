using UnrealBuildTool;
using System.Collections.Generic;

public class BounceTarget : TargetRules
{
	public BounceTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Bounce");
	}
}

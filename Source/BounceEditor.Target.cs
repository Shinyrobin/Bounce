using UnrealBuildTool;
using System.Collections.Generic;

public class BounceEditorTarget : TargetRules
{
	public BounceEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "Bounce", "BounceEditor" });
	}
}

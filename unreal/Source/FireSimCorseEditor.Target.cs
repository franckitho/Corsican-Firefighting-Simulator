using UnrealBuildTool;

public class FireSimCorseEditorTarget : TargetRules
{
	public FireSimCorseEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("FireSimCorse");
	}
}

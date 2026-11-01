using UnrealBuildTool;

public class FireSimCorseTarget : TargetRules
{
	public FireSimCorseTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("FireSimCorse");
	}
}

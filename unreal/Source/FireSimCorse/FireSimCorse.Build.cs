using UnrealBuildTool;

// Module du projet : vide pour l'instant, tout le simulateur est dans le plugin FireSim.
public class FireSimCorse : ModuleRules
{
	public FireSimCorse(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
	}
}

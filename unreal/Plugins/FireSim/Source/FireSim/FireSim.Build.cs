using System.IO;
using UnrealBuildTool;

// Le module de simulation du dépôt (sim/, C++20 sans dépendance) est compilé directement dans ce module,
// avec la chaîne d'Unreal : chaque fichier de Private/Core inclut un fichier de sim/src.
public class FireSim : ModuleRules
{
	public FireSim(ReadOnlyTargetRules Target) : base(Target)
	{
		string SimDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", "..", "..", "sim"));
		PublicIncludePaths.Add(Path.Combine(SimDir, "include"));
		PrivateIncludePaths.Add(SimDir);

		// Les fichiers de sim/src ne doivent recevoir ni en-têtes précompilés d'Unreal, ni être fusionnés
		// entre eux (espaces de noms anonymes aux noms communs).
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		// Le module de simulation signale ses erreurs (départ dans l'eau, fichier manquant) par des exceptions.
		bEnableExceptions = true;

		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "CesiumRuntime" });
		// Slate : panneau de réglage affiché en jeu ; HTTP : météo réelle téléchargée sur Open-Meteo
		PrivateDependencyModuleNames.AddRange(new[] { "Json", "Slate", "SlateCore", "HTTP" });
	}
}

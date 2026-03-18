using System.IO;
using UnrealBuildTool;

public class SofaPhysicsAPILib : ModuleRules
{
	public SofaPhysicsAPILib(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Headers
			PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

			// Import lib
			string SofaBuildLib = @"D:/sofa/build/lib/Release/SofaPhysicsAPI.lib";
			PublicAdditionalLibraries.Add(SofaBuildLib);

			// Copy all SOFA DLLs to output directory at runtime
			string SofaBinDir = @"D:/sofa/build/bin/Release";
			foreach (string dll in Directory.GetFiles(SofaBinDir, "*.dll"))
			{
				string fileName = Path.GetFileName(dll);
				RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", fileName), dll);
			}

			// Force Dimensions SDK DLL (for OmegaDriver)
			string FdDll = @"C:/Program Files/Force Dimension/sdk-3.17.6/bin/dhd64.dll";
			if (File.Exists(FdDll))
			{
				RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", "dhd64.dll"), FdDll);
			}
		}
	}
}

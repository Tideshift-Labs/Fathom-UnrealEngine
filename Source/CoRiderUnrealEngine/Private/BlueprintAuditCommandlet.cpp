#include "BlueprintAuditCommandlet.h"

#include "BlueprintAuditor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UBlueprintAuditCommandlet::UBlueprintAuditCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UBlueprintAuditCommandlet::Main(const FString& Params)
{
	UE_LOG(LogBlueprintAudit, Display, TEXT("=== Blueprint Audit ==="));

	// Parse parameters
	FString AssetPath;
	FParse::Value(*Params, TEXT("-AssetPath="), AssetPath);

	FString OutputPath;
	FParse::Value(*Params, TEXT("-Output="), OutputPath);

	// Initialize asset registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.SearchAllAssets(true);

	// --- Single-asset mode: write one combined JSON file ---
	if (!AssetPath.IsEmpty())
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!BP)
		{
			// Try appending asset name for package-style paths like /Game/UI/WBP_Foo
			const FString AssetName = FPackageName::GetShortName(AssetPath);
			const FString FullPath = AssetPath + TEXT(".") + AssetName;
			BP = LoadObject<UBlueprint>(nullptr, *FullPath);
		}

		if (!BP)
		{
			UE_LOG(LogBlueprintAudit, Error, TEXT("Could not load blueprint: %s"), *AssetPath);
			return 1;
		}

		if (OutputPath.IsEmpty())
		{
			OutputPath = FPaths::ProjectDir() / TEXT("BlueprintAudit.json");
		}

		UE_LOG(LogBlueprintAudit, Display, TEXT("Auditing 1 blueprint..."));

		TSharedPtr<FJsonObject> AuditJson = FBlueprintAuditor::AuditBlueprint(BP);
		if (!FBlueprintAuditor::WriteAuditJson(AuditJson, OutputPath))
		{
			return 1;
		}

		UE_LOG(LogBlueprintAudit, Display, TEXT("=== Blueprint Audit Complete ==="));
		return 0;
	}

	// --- All-assets mode: write per-file JSONs under Saved/Audit/Blueprints/ ---
	TArray<FAssetData> AllBlueprints;
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

	UE_LOG(LogBlueprintAudit, Display, TEXT("Auditing %d blueprint(s)..."), AllBlueprints.Num());

	int32 SuccessCount = 0;
	for (const FAssetData& Asset : AllBlueprints)
	{
		// Filter: Only audit project content (starts with /Game/)
		if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP)
		{
			continue;
		}

		const FString PerFilePath = FBlueprintAuditor::GetAuditOutputPath(BP);
		TSharedPtr<FJsonObject> AuditJson = FBlueprintAuditor::AuditBlueprint(BP);
		if (FBlueprintAuditor::WriteAuditJson(AuditJson, PerFilePath))
		{
			++SuccessCount;
		}
	}

	UE_LOG(LogBlueprintAudit, Display, TEXT("Wrote %d audit file(s)."), SuccessCount);
	UE_LOG(LogBlueprintAudit, Display, TEXT("=== Blueprint Audit Complete ==="));
	return 0;
}

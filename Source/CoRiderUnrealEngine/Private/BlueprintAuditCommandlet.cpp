#include "BlueprintAuditCommandlet.h"

#include "BlueprintAuditor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"

UBlueprintAuditCommandlet::UBlueprintAuditCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UBlueprintAuditCommandlet::Main(const FString& Params)
{
	// Parse parameters
	FString AssetPath;
	FParse::Value(*Params, TEXT("-AssetPath="), AssetPath);

	FString OutputPath;
	FParse::Value(*Params, TEXT("-Output="), OutputPath);

	// Initialize asset registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.SearchAllAssets(true);

	// --- Single-asset mode: write one audit file ---
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
			UE_LOG(LogCoRider, Error, TEXT("CoRider: Blueprint not found: %s"), *AssetPath);
			return 1;
		}

		if (OutputPath.IsEmpty())
		{
			OutputPath = FPaths::ProjectDir() / TEXT("BlueprintAudit.md");
		}

		UE_LOG(LogCoRider, Display, TEXT("CoRider: Auditing 1 Blueprint..."));

		const double StartTime = FPlatformTime::Seconds();
		const FString AuditMarkdown = FBlueprintAuditor::AuditBlueprint(BP);
		if (!FBlueprintAuditor::WriteAuditFile(AuditMarkdown, OutputPath))
		{
			return 1;
		}
		const double Elapsed = FPlatformTime::Seconds() - StartTime;

		UE_LOG(LogCoRider, Display, TEXT("CoRider: Audit complete, wrote %s in %.2fs"), *OutputPath, Elapsed);
		return 0;
	}

	// --- All-assets mode: write per-file audit under Saved/Audit/Blueprints/ ---
	TArray<FAssetData> AllBlueprints;
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

	UE_LOG(LogCoRider, Display, TEXT("CoRider: Auditing %d Blueprint(s)..."), AllBlueprints.Num());

	const double StartTime = FPlatformTime::Seconds();
	int32 SuccessCount = 0;
	int32 SkipCount = 0;
	int32 FailCount = 0;

	int32 AssetsSinceGC = 0;
	constexpr int32 GCInterval = 50;

	for (const FAssetData& Asset : AllBlueprints)
	{
		// Filter: Only audit project content (starts with /Game/)
		if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
		{
			++SkipCount;
			continue;
		}

		if (!FBlueprintAuditor::IsSupportedBlueprintClass(Asset.AssetClassPath))
		{
			++SkipCount;
			UE_LOG(LogCoRider, Verbose, TEXT("CoRider: Skipping unsupported Blueprint class %s (%s)"),
				*Asset.PackageName.ToString(), *Asset.AssetClassPath.ToString());
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP)
		{
			++FailCount;
			UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to load asset %s"), *Asset.PackageName.ToString());
			continue;
		}

		const FString PerFilePath = FBlueprintAuditor::GetAuditOutputPath(BP);
		const FString AuditMarkdown = FBlueprintAuditor::AuditBlueprint(BP);
		if (FBlueprintAuditor::WriteAuditFile(AuditMarkdown, PerFilePath))
		{
			++SuccessCount;
		}
		else
		{
			++FailCount;
			UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to write audit for %s"), *BP->GetName());
		}

		if (++AssetsSinceGC >= GCInterval)
		{
			CollectGarbage(RF_NoFlags);
			AssetsSinceGC = 0;
		}
	}

	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogCoRider, Display, TEXT("CoRider: Audit complete, %d written, %d skipped, %d failed in %.2fs"),
		SuccessCount, SkipCount, FailCount, Elapsed);
	return 0;
}

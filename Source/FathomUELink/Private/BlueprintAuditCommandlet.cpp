#include "BlueprintAuditCommandlet.h"

#include "BlueprintAuditor.h"
#include "FathomUELinkModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ControlRigBlueprintLegacy.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "StructUtils/UserDefinedStruct.h"
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
			UE_LOG(LogFathomUELink, Error, TEXT("Fathom: Blueprint not found: %s"), *AssetPath);
			return 1;
		}

		if (OutputPath.IsEmpty())
		{
			OutputPath = FPaths::ProjectDir() / TEXT("BlueprintAudit.md");
		}

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Auditing 1 Blueprint..."));

		const double StartTime = FPlatformTime::Seconds();
		const FString AuditMarkdown = FBlueprintAuditor::AuditBlueprint(BP);
		if (!FBlueprintAuditor::WriteAuditFile(AuditMarkdown, OutputPath))
		{
			return 1;
		}
		const double Elapsed = FPlatformTime::Seconds() - StartTime;

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Audit complete, wrote %s in %.2fs"), *OutputPath, Elapsed);
		return 0;
	}

	// --- All-assets mode: write per-file audit under Saved/Fathom/Audit/Blueprints/ ---
	TArray<FAssetData> AllBlueprints;
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

	UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Auditing %d Blueprint(s)..."), AllBlueprints.Num());

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
			UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Skipping unsupported Blueprint class %s (%s)"),
				*Asset.PackageName.ToString(), *Asset.AssetClassPath.ToString());
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP)
		{
			++FailCount;
			UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load asset %s"), *Asset.PackageName.ToString());
			continue;
		}

		FString PerFilePath;
		FString AuditMarkdown;
		if (const UControlRigBlueprint* CRBP = Cast<UControlRigBlueprint>(BP))
		{
			FControlRigAuditData Data = FBlueprintAuditor::GatherControlRigData(CRBP);
			PerFilePath = Data.OutputPath;
			AuditMarkdown = FBlueprintAuditor::SerializeControlRigToMarkdown(Data);
		}
		else
		{
			PerFilePath = FBlueprintAuditor::GetAuditOutputPath(BP);
			AuditMarkdown = FBlueprintAuditor::AuditBlueprint(BP);
		}
		if (FBlueprintAuditor::WriteAuditFile(AuditMarkdown, PerFilePath))
		{
			++SuccessCount;
		}
		else
		{
			++FailCount;
			UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to write audit for %s"), *BP->GetName());
		}

		if (++AssetsSinceGC >= GCInterval)
		{
			CollectGarbage(RF_NoFlags);
			AssetsSinceGC = 0;
		}
	}

	// --- DataTable batch ---
	{
		TArray<FAssetData> AllDataTables;
		AssetRegistry.GetAssetsByClass(UDataTable::StaticClass()->GetClassPathName(), AllDataTables, false);

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Auditing %d DataTable(s)..."), AllDataTables.Num());

		for (const FAssetData& Asset : AllDataTables)
		{
			if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
			{
				++SkipCount;
				continue;
			}

			const UDataTable* DT = Cast<UDataTable>(Asset.GetAsset());
			if (!DT)
			{
				++FailCount;
				UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load DataTable %s"), *Asset.PackageName.ToString());
				continue;
			}

			FDataTableAuditData Data = FBlueprintAuditor::GatherDataTableData(DT);
			const FString Markdown = FBlueprintAuditor::SerializeDataTableToMarkdown(Data);
			if (FBlueprintAuditor::WriteAuditFile(Markdown, Data.OutputPath))
			{
				++SuccessCount;
			}
			else
			{
				++FailCount;
			}

			if (++AssetsSinceGC >= GCInterval)
			{
				CollectGarbage(RF_NoFlags);
				AssetsSinceGC = 0;
			}
		}
	}

	// --- DataAsset batch ---
	{
		TArray<FAssetData> AllDataAssets;
		AssetRegistry.GetAssetsByClass(UDataAsset::StaticClass()->GetClassPathName(), AllDataAssets, true);

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Auditing %d DataAsset(s)..."), AllDataAssets.Num());

		for (const FAssetData& Asset : AllDataAssets)
		{
			if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
			{
				++SkipCount;
				continue;
			}

			const UDataAsset* DA = Cast<UDataAsset>(Asset.GetAsset());
			if (!DA)
			{
				++FailCount;
				UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load DataAsset %s"), *Asset.PackageName.ToString());
				continue;
			}

			FDataAssetAuditData Data = FBlueprintAuditor::GatherDataAssetData(DA);
			const FString Markdown = FBlueprintAuditor::SerializeDataAssetToMarkdown(Data);
			if (FBlueprintAuditor::WriteAuditFile(Markdown, Data.OutputPath))
			{
				++SuccessCount;
			}
			else
			{
				++FailCount;
			}

			if (++AssetsSinceGC >= GCInterval)
			{
				CollectGarbage(RF_NoFlags);
				AssetsSinceGC = 0;
			}
		}
	}

	// --- UserDefinedStruct batch ---
	{
		TArray<FAssetData> AllStructs;
		AssetRegistry.GetAssetsByClass(UUserDefinedStruct::StaticClass()->GetClassPathName(), AllStructs, false);

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Auditing %d UserDefinedStruct(s)..."), AllStructs.Num());

		for (const FAssetData& Asset : AllStructs)
		{
			if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
			{
				++SkipCount;
				continue;
			}

			const UUserDefinedStruct* UDS = Cast<UUserDefinedStruct>(Asset.GetAsset());
			if (!UDS)
			{
				++FailCount;
				UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load UserDefinedStruct %s"), *Asset.PackageName.ToString());
				continue;
			}

			FUserDefinedStructAuditData Data = FBlueprintAuditor::GatherUserDefinedStructData(UDS);
			const FString Markdown = FBlueprintAuditor::SerializeUserDefinedStructToMarkdown(Data);
			if (FBlueprintAuditor::WriteAuditFile(Markdown, Data.OutputPath))
			{
				++SuccessCount;
			}
			else
			{
				++FailCount;
			}

			if (++AssetsSinceGC >= GCInterval)
			{
				CollectGarbage(RF_NoFlags);
				AssetsSinceGC = 0;
			}
		}
	}

	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Audit complete, %d written, %d skipped, %d failed in %.2fs"),
		SuccessCount, SkipCount, FailCount, Elapsed);
	return 0;
}

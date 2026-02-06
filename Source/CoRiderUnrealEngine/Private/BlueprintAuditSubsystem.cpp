#include "BlueprintAuditSubsystem.h"

#include "BlueprintAuditor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectIterator.h"

void UBlueprintAuditSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UPackage::PackageSavedWithContextEvent.AddUObject(this, &UBlueprintAuditSubsystem::OnPackageSaved);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.OnAssetRemoved().AddUObject(this, &UBlueprintAuditSubsystem::OnAssetRemoved);
	AssetRegistry.OnAssetRenamed().AddUObject(this, &UBlueprintAuditSubsystem::OnAssetRenamed);

	// Schedule a deferred stale-check for after the asset registry finishes loading
	StaleCheckTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UBlueprintAuditSubsystem::OnStaleCheckTick));

	UE_LOG(LogCoRider, Display, TEXT("CoRider: Subsystem initialized, watching for Blueprint saves."));
}

void UBlueprintAuditSubsystem::Deinitialize()
{
	if (StaleCheckTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StaleCheckTickerHandle);
		StaleCheckTickerHandle.Reset();
	}

	UPackage::PackageSavedWithContextEvent.RemoveAll(this);

	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		IAssetRegistry& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		AssetRegistry.OnAssetRemoved().RemoveAll(this);
		AssetRegistry.OnAssetRenamed().RemoveAll(this);
	}

	UE_LOG(LogCoRider, Log, TEXT("CoRider: Subsystem deinitialized."));

	Super::Deinitialize();
}

void UBlueprintAuditSubsystem::OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext ObjectSaveContext)
{
	if (!Package)
	{
		return;
	}

	// Skip procedural/cook saves — only audit interactive editor saves
	if (ObjectSaveContext.IsCooking() || ObjectSaveContext.IsProceduralSave())
	{
		return;
	}

	// Filter: Only audit project content (starts with /Game/)
	if (!Package->GetName().StartsWith(TEXT("/Game/")))
	{
		return;
	}

	// Walk all objects in the saved package, looking for Blueprints
	ForEachObjectWithPackage(Package, [](UObject* Object)
	{
		if (const UBlueprint* BP = Cast<UBlueprint>(Object))
		{
			UE_LOG(LogCoRider, Verbose, TEXT("CoRider: Auditing saved Blueprint %s"), *BP->GetName());
			const FString OutputPath = FBlueprintAuditor::GetAuditOutputPath(BP);
			const TSharedPtr<FJsonObject> AuditJson = FBlueprintAuditor::AuditBlueprint(BP);
			FBlueprintAuditor::WriteAuditJson(AuditJson, OutputPath);
		}
		return true; // continue iteration
	});
}

void UBlueprintAuditSubsystem::OnAssetRemoved(const FAssetData& AssetData)
{
	const FString PackageName = AssetData.PackageName.ToString();
	if (!PackageName.StartsWith(TEXT("/Game/")))
	{
		return;
	}

	if (!AssetData.IsInstanceOf(UBlueprint::StaticClass()))
	{
		return;
	}

	const FString JsonPath = FBlueprintAuditor::GetAuditOutputPath(PackageName);
	FBlueprintAuditor::DeleteAuditJson(JsonPath);
}

void UBlueprintAuditSubsystem::OnAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
	if (!AssetData.IsInstanceOf(UBlueprint::StaticClass()))
	{
		return;
	}

	const FString OldPackageName = FPackageName::ObjectPathToPackageName(OldObjectPath);
	if (!OldPackageName.StartsWith(TEXT("/Game/")))
	{
		return;
	}

	const FString OldJsonPath = FBlueprintAuditor::GetAuditOutputPath(OldPackageName);
	FBlueprintAuditor::DeleteAuditJson(OldJsonPath);
}

bool UBlueprintAuditSubsystem::OnStaleCheckTick(float DeltaTime)
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (AssetRegistry.IsLoadingAssets())
	{
		UE_LOG(LogCoRider, Verbose, TEXT("CoRider: Asset registry still loading, deferring stale check..."));
		return true;
	}

	AuditStaleBlueprints();

	// Return false to unregister — this is a one-shot check
	StaleCheckTickerHandle.Reset();
	return false;
}

void UBlueprintAuditSubsystem::AuditStaleBlueprints()
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> AllBlueprints;
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

	const double StartTime = FPlatformTime::Seconds();
	int32 TotalScanned = 0;
	int32 UpToDateCount = 0;
	int32 ReAuditedCount = 0;
	int32 FailedCount = 0;

	for (const FAssetData& Asset : AllBlueprints)
	{
		const FString PackageName = Asset.PackageName.ToString();

		// Filter: Only audit project content (starts with /Game/)
		if (!PackageName.StartsWith(TEXT("/Game/")))
		{
			continue;
		}

		++TotalScanned;

		const FString JsonPath = FBlueprintAuditor::GetAuditOutputPath(PackageName);
		const FString SourcePath = FBlueprintAuditor::GetSourceFilePath(PackageName);

		if (SourcePath.IsEmpty())
		{
			++FailedCount;
			continue;
		}

		// Compute current .uasset hash
		const FString CurrentHash = FBlueprintAuditor::ComputeFileHash(SourcePath);
		if (CurrentHash.IsEmpty())
		{
			++FailedCount;
			continue;
		}

		// Read stored hash from existing JSON (if any)
		FString StoredHash;
		FString JsonString;
		if (FFileHelper::LoadFileToString(JsonString, *JsonPath))
		{
			TSharedPtr<FJsonObject> ExistingJson;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
			if (FJsonSerializer::Deserialize(Reader, ExistingJson) && ExistingJson.IsValid())
			{
				StoredHash = ExistingJson->GetStringField(TEXT("SourceFileHash"));
			}
			else
			{
				UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to parse existing audit JSON for %s"), *PackageName);
			}
		}

		// Skip if hash matches — this Blueprint is up to date
		if (CurrentHash == StoredHash)
		{
			UE_LOG(LogCoRider, Verbose, TEXT("CoRider: %s is up-to-date, skipping"), *PackageName);
			++UpToDateCount;
			continue;
		}

		// Stale or missing — load the Blueprint and re-audit
		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP)
		{
			++FailedCount;
			UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to load asset %s for re-audit"), *PackageName);
			continue;
		}

		const TSharedPtr<FJsonObject> AuditJson = FBlueprintAuditor::AuditBlueprint(BP);
		FBlueprintAuditor::WriteAuditJson(AuditJson, JsonPath);
		++ReAuditedCount;
	}

	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogCoRider, Display, TEXT("CoRider: Stale check complete: %d scanned, %d up-to-date, %d re-audited, %d failed in %.2fs"),
		TotalScanned, UpToDateCount, ReAuditedCount, FailedCount, Elapsed);

	SweepOrphanedAuditFiles();
}

void UBlueprintAuditSubsystem::SweepOrphanedAuditFiles()
{
	const FString BaseDir = FBlueprintAuditor::GetAuditBaseDir();

	TArray<FString> JsonFiles;
	IFileManager::Get().FindFilesRecursive(JsonFiles, *BaseDir, TEXT("*.json"), true, false);

	if (JsonFiles.IsEmpty())
	{
		return;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	int32 SweptCount = 0;
	for (const FString& JsonFile : JsonFiles)
	{
		// Convert absolute path back to a package name:
		// Strip the base dir prefix and .json suffix, then prepend /Game/
		FString RelPath = JsonFile;
		if (!RelPath.StartsWith(BaseDir))
		{
			continue;
		}
		RelPath.RightChopInline(BaseDir.Len());

		// Remove leading separator if present
		if (RelPath.StartsWith(TEXT("/")) || RelPath.StartsWith(TEXT("\\")))
		{
			RelPath.RightChopInline(1);
		}

		// Remove .json suffix
		if (RelPath.EndsWith(TEXT(".json")))
		{
			RelPath.LeftChopInline(5);
		}

		// Normalize separators for the package path
		RelPath.ReplaceInline(TEXT("\\"), TEXT("/"));

		const FString PackageName = TEXT("/Game/") + RelPath;

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets, true);
		if (Assets.IsEmpty())
		{
			FBlueprintAuditor::DeleteAuditJson(JsonFile);
			++SweptCount;
		}
	}

	if (SweptCount > 0)
	{
		UE_LOG(LogCoRider, Display, TEXT("CoRider: Swept %d orphaned audit file(s)"), SweptCount);
	}
}

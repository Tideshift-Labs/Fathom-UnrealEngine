#include "BlueprintAuditSubsystem.h"

#include "BlueprintAuditor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectIterator.h"

void UBlueprintAuditSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UPackage::PackageSavedWithContextEvent.AddUObject(this, &UBlueprintAuditSubsystem::OnPackageSaved);

	// Schedule a deferred stale-check for after the asset registry finishes loading
	StaleCheckTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UBlueprintAuditSubsystem::OnStaleCheckTick));

	UE_LOG(LogBlueprintAudit, Display, TEXT("BlueprintAuditSubsystem initialized — watching for Blueprint saves."));
}

void UBlueprintAuditSubsystem::Deinitialize()
{
	if (StaleCheckTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StaleCheckTickerHandle);
		StaleCheckTickerHandle.Reset();
	}

	UPackage::PackageSavedWithContextEvent.RemoveAll(this);

	UE_LOG(LogBlueprintAudit, Display, TEXT("BlueprintAuditSubsystem deinitialized."));

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
			const FString OutputPath = FBlueprintAuditor::GetAuditOutputPath(BP);
			const TSharedPtr<FJsonObject> AuditJson = FBlueprintAuditor::AuditBlueprint(BP);
			FBlueprintAuditor::WriteAuditJson(AuditJson, OutputPath);
		}
		return true; // continue iteration
	});
}

bool UBlueprintAuditSubsystem::OnStaleCheckTick(float DeltaTime)
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (AssetRegistry.IsLoadingAssets())
	{
		// Asset registry still scanning — keep ticking until it's ready
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

	int32 AuditedCount = 0;

	for (const FAssetData& Asset : AllBlueprints)
	{
		const FString PackageName = Asset.PackageName.ToString();

		// Filter: Only audit project content (starts with /Game/)
		if (!PackageName.StartsWith(TEXT("/Game/")))
		{
			continue;
		}

		const FString JsonPath = FBlueprintAuditor::GetAuditOutputPath(PackageName);
		const FString SourcePath = FBlueprintAuditor::GetSourceFilePath(PackageName);

		if (SourcePath.IsEmpty())
		{
			continue;
		}

		// Compute current .uasset hash
		const FString CurrentHash = FBlueprintAuditor::ComputeFileHash(SourcePath);
		if (CurrentHash.IsEmpty())
		{
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
		}

		// Skip if hash matches — this Blueprint is up to date
		if (CurrentHash == StoredHash)
		{
			continue;
		}

		// Stale or missing — load the Blueprint and re-audit
		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> AuditJson = FBlueprintAuditor::AuditBlueprint(BP);
		FBlueprintAuditor::WriteAuditJson(AuditJson, JsonPath);
		++AuditedCount;
	}

	UE_LOG(LogBlueprintAudit, Display, TEXT("Stale check complete — re-audited %d / %d Blueprint(s)."),
		AuditedCount, AllBlueprints.Num());
}

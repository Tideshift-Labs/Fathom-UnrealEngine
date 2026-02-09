#include "BlueprintAuditSubsystem.h"

#include "BlueprintAuditor.h"
#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/ObjectSaveContext.h"

void UBlueprintAuditSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UPackage::PackageSavedWithContextEvent.AddUObject(this, &UBlueprintAuditSubsystem::OnPackageSaved);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.OnAssetRemoved().AddUObject(this, &UBlueprintAuditSubsystem::OnAssetRemoved);
	AssetRegistry.OnAssetRenamed().AddUObject(this, &UBlueprintAuditSubsystem::OnAssetRenamed);

	// Schedule the stale-check state machine
	StaleCheckPhase = EStaleCheckPhase::WaitingForRegistry;
	StaleCheckTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UBlueprintAuditSubsystem::OnStaleCheckTick));

	UE_LOG(LogCoRider, Display, TEXT("CoRider: Subsystem initialized, watching for Blueprint saves."));
}

void UBlueprintAuditSubsystem::Deinitialize()
{
	// 1. Remove ticker (prevents new ticks)
	if (StaleCheckTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StaleCheckTickerHandle);
		StaleCheckTickerHandle.Reset();
	}

	// 2. Remove event delegates (prevents new OnPackageSaved calls)
	UPackage::PackageSavedWithContextEvent.RemoveAll(this);

	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		IAssetRegistry& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		AssetRegistry.OnAssetRemoved().RemoveAll(this);
		AssetRegistry.OnAssetRenamed().RemoveAll(this);
	}

	// 3. Wait on all background futures with a 5-second total timeout
	const double WaitStart = FPlatformTime::Seconds();
	constexpr double TimeoutSec = 5.0;

	if (Phase2Future.IsValid())
	{
		const double Remaining = TimeoutSec - (FPlatformTime::Seconds() - WaitStart);
		if (Remaining > 0)
		{
			Phase2Future.WaitFor(FTimespan::FromSeconds(Remaining));
		}
	}

	for (TFuture<void>& F : PendingFutures)
	{
		if (F.IsValid())
		{
			const double Remaining = TimeoutSec - (FPlatformTime::Seconds() - WaitStart);
			if (Remaining > 0)
			{
				F.WaitFor(FTimespan::FromSeconds(Remaining));
			}
		}
	}

	const double WaitElapsed = FPlatformTime::Seconds() - WaitStart;
	if (WaitElapsed >= TimeoutSec)
	{
		UE_LOG(LogCoRider, Warning, TEXT("CoRider: Shutdown timed out after %.1fs waiting for background tasks"), WaitElapsed);
	}

	PendingFutures.Empty();

	UE_LOG(LogCoRider, Log, TEXT("CoRider: Subsystem deinitialized."));

	Super::Deinitialize();
}

void UBlueprintAuditSubsystem::OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext ObjectSaveContext)
{
	if (!Package)
	{
		return;
	}

	// Skip procedural/cook saves
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
	ForEachObjectWithPackage(Package, [this](UObject* Object)
	{
		if (const UBlueprint* BP = Cast<UBlueprint>(Object))
		{
			// Gather data on the game thread (fast, reads UObject pointers)
			FBlueprintAuditData Data = FBlueprintAuditor::GatherBlueprintData(BP);

			// Check dedup: skip if already in-flight
			{
				FScopeLock Lock(&InFlightLock);
				if (InFlightPackages.Contains(Data.PackageName))
				{
					UE_LOG(LogCoRider, Verbose, TEXT("CoRider: %s already in-flight, skipping"), *Data.PackageName);
					return true;
				}
			}

			UE_LOG(LogCoRider, Verbose, TEXT("CoRider: Dispatching async audit for saved Blueprint %s"), *Data.Name);
			DispatchBackgroundWrite(MoveTemp(Data));
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
	switch (StaleCheckPhase)
	{
	case EStaleCheckPhase::WaitingForRegistry:
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		if (AssetRegistry.IsLoadingAssets())
		{
			UE_LOG(LogCoRider, Verbose, TEXT("CoRider: Asset registry still loading, deferring stale check..."));
			return true; // keep ticking
		}

		StaleCheckPhase = EStaleCheckPhase::BuildingList;
		return true;
	}

	case EStaleCheckPhase::BuildingList:
	{
		StaleCheckStartTime = FPlatformTime::Seconds();

		// Query the asset registry for all /Game/ Blueprints
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> AllBlueprints;
		AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

		StaleCheckEntries.Reset();
		StaleCheckEntries.Reserve(AllBlueprints.Num());

		for (const FAssetData& Asset : AllBlueprints)
		{
			const FString PackageName = Asset.PackageName.ToString();
			if (!PackageName.StartsWith(TEXT("/Game/")))
			{
				continue;
			}

			FStaleCheckEntry Entry;
			Entry.PackageName = PackageName;
			Entry.SourcePath = FBlueprintAuditor::GetSourceFilePath(PackageName);
			Entry.JsonPath = FBlueprintAuditor::GetAuditOutputPath(PackageName);
			StaleCheckEntries.Add(MoveTemp(Entry));
		}

		UE_LOG(LogCoRider, Display, TEXT("CoRider: Stale check Phase 1 complete: %d Blueprints to check"), StaleCheckEntries.Num());

		// Dispatch Phase 2 to a background thread: hash comparison
		// Copy the entries for the background thread (no UObject pointers)
		TArray<FStaleCheckEntry> EntriesCopy = StaleCheckEntries;
		Phase2Future = Async(EAsyncExecution::ThreadPool, [Entries = MoveTemp(EntriesCopy)]() -> TArray<FString>
		{
			TArray<FString> StaleNames;

			for (const FStaleCheckEntry& Entry : Entries)
			{
				if (Entry.SourcePath.IsEmpty())
				{
					continue;
				}

				// Compute current .uasset hash
				const FString CurrentHash = FBlueprintAuditor::ComputeFileHash(Entry.SourcePath);
				if (CurrentHash.IsEmpty())
				{
					continue;
				}

				// Read stored hash from existing JSON (if any)
				FString StoredHash;
				FString JsonString;
				if (FFileHelper::LoadFileToString(JsonString, *Entry.JsonPath))
				{
					TSharedPtr<FJsonObject> ExistingJson;
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
					if (FJsonSerializer::Deserialize(Reader, ExistingJson) && ExistingJson.IsValid())
					{
						StoredHash = ExistingJson->GetStringField(TEXT("SourceFileHash"));
					}
				}

				if (CurrentHash != StoredHash)
				{
					StaleNames.Add(Entry.PackageName);
				}
			}

			return StaleNames;
		});

		StaleCheckPhase = EStaleCheckPhase::BackgroundHash;
		return true;
	}

	case EStaleCheckPhase::BackgroundHash:
	{
		if (!Phase2Future.IsReady())
		{
			return true; // keep polling
		}

		StalePackageNames = Phase2Future.Get();
		StaleProcessIndex = 0;
		StaleReAuditedCount = 0;
		StaleFailedCount = 0;
		AssetsSinceGC = 0;

		UE_LOG(LogCoRider, Display, TEXT("CoRider: Stale check Phase 2 complete: %d stale Blueprint(s) to re-audit"), StalePackageNames.Num());

		if (StalePackageNames.Num() == 0)
		{
			StaleCheckPhase = EStaleCheckPhase::Done;
			return true;
		}

		StaleCheckPhase = EStaleCheckPhase::ProcessingStale;
		return true;
	}

	case EStaleCheckPhase::ProcessingStale:
	{
		// Process a batch of stale Blueprints per tick
		const int32 BatchEnd = FMath::Min(StaleProcessIndex + StaleProcessBatchSize, StalePackageNames.Num());

		for (int32 i = StaleProcessIndex; i < BatchEnd; ++i)
		{
			const FString& PackageName = StalePackageNames[i];

			// Load the Blueprint on the game thread
			const FString AssetPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
			if (!BP)
			{
				++StaleFailedCount;
				UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to load asset %s for re-audit"), *PackageName);
				continue;
			}

			// Gather data on the game thread, dispatch write to background
			FBlueprintAuditData Data = FBlueprintAuditor::GatherBlueprintData(BP);
			DispatchBackgroundWrite(MoveTemp(Data));
			++StaleReAuditedCount;

			if (++AssetsSinceGC >= GCInterval)
			{
				CollectGarbage(RF_NoFlags);
				AssetsSinceGC = 0;
			}
		}

		StaleProcessIndex = BatchEnd;

		if (StaleProcessIndex >= StalePackageNames.Num())
		{
			StaleCheckPhase = EStaleCheckPhase::Done;
		}

		return true;
	}

	case EStaleCheckPhase::Done:
	{
		const double Elapsed = FPlatformTime::Seconds() - StaleCheckStartTime;
		UE_LOG(LogCoRider, Display, TEXT("CoRider: Stale check complete: %d scanned, %d re-audited, %d failed in %.2fs"),
			StaleCheckEntries.Num(), StaleReAuditedCount, StaleFailedCount, Elapsed);

		SweepOrphanedAuditFiles();

		// Clean up state
		StaleCheckEntries.Empty();
		StalePackageNames.Empty();
		StaleCheckPhase = EStaleCheckPhase::Idle;
		StaleCheckTickerHandle.Reset();
		return false; // unregister ticker
	}

	default:
		return false;
	}
}

void UBlueprintAuditSubsystem::DispatchBackgroundWrite(FBlueprintAuditData&& Data)
{
	const FString PackageName = Data.PackageName;
	const FString OutputPath = Data.OutputPath;

	// Add to in-flight set (under lock)
	{
		FScopeLock Lock(&InFlightLock);
		InFlightPackages.Add(PackageName);
	}

	CleanupCompletedFutures();

	// Capture Data by move, PackageName/OutputPath by copy for the lambda
	TFuture<void> Future = Async(EAsyncExecution::ThreadPool,
		[this, MovedData = MoveTemp(Data), PackageName, OutputPath]()
		{
			const TSharedPtr<FJsonObject> Json = FBlueprintAuditor::SerializeToJson(MovedData);
			FBlueprintAuditor::WriteAuditJson(Json, OutputPath);

			// Remove from in-flight set
			FScopeLock Lock(&InFlightLock);
			InFlightPackages.Remove(PackageName);
		});

	PendingFutures.Add(MoveTemp(Future));
}

void UBlueprintAuditSubsystem::CleanupCompletedFutures()
{
	PendingFutures.RemoveAll([](const TFuture<void>& F)
	{
		return F.IsReady();
	});
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

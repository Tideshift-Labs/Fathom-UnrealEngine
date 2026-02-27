#include "BlueprintAuditSubsystem.h"

#include "BlueprintAuditor.h"
#include "FathomUELinkModule.h"
#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "StructUtils/UserDefinedStruct.h"
#include "ControlRigBlueprint.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Audit/AuditFileUtils.h"
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

	FAuditFileUtils::WriteAuditManifest();

	UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Subsystem initialized, watching for Blueprint saves."));
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
		UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Shutdown timed out after %.1fs waiting for background tasks"), WaitElapsed);
	}

	PendingFutures.Empty();

	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: Subsystem deinitialized."));

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

	// Walk all objects in the saved package, looking for auditable assets
	ForEachObjectWithPackage(Package, [this](UObject* Object)
	{
		if (const UControlRigBlueprint* CRBP = Cast<UControlRigBlueprint>(Object))
		{
			FControlRigAuditData Data = FBlueprintAuditor::GatherControlRigData(CRBP);

			{
				FScopeLock Lock(&InFlightLock);
				if (InFlightPackages.Contains(Data.PackageName))
				{
					return true;
				}
			}

			UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Dispatching async audit for saved ControlRig %s"), *Data.Name);
			DispatchBackgroundWrite(MoveTemp(Data));
		}
		else if (const UBlueprint* BP = Cast<UBlueprint>(Object))
		{
			if (!FBlueprintAuditor::IsSupportedBlueprintClass(BP->GetClass()->GetClassPathName()))
			{
				return true; // skip unsupported Blueprint subclasses
			}
			FBlueprintAuditData Data = FBlueprintAuditor::GatherBlueprintData(BP);

			{
				FScopeLock Lock(&InFlightLock);
				if (InFlightPackages.Contains(Data.PackageName))
				{
					UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: %s already in-flight, skipping"), *Data.PackageName);
					return true;
				}
			}

			UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Dispatching async audit for saved Blueprint %s"), *Data.Name);
			DispatchBackgroundWrite(MoveTemp(Data));
		}
		else if (const UDataTable* DT = Cast<UDataTable>(Object))
		{
			FDataTableAuditData Data = FBlueprintAuditor::GatherDataTableData(DT);

			{
				FScopeLock Lock(&InFlightLock);
				if (InFlightPackages.Contains(Data.PackageName))
				{
					return true;
				}
			}

			UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Dispatching async audit for saved DataTable %s"), *Data.Name);
			DispatchBackgroundWrite(MoveTemp(Data));
		}
		else if (const UUserDefinedStruct* UDS = Cast<UUserDefinedStruct>(Object))
		{
			FUserDefinedStructAuditData Data = FBlueprintAuditor::GatherUserDefinedStructData(UDS);

			{
				FScopeLock Lock(&InFlightLock);
				if (InFlightPackages.Contains(Data.PackageName))
				{
					return true;
				}
			}

			UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Dispatching async audit for saved UserDefinedStruct %s"), *Data.Name);
			DispatchBackgroundWrite(MoveTemp(Data));
		}
		else if (const UDataAsset* DA = Cast<UDataAsset>(Object))
		{
			FDataAssetAuditData Data = FBlueprintAuditor::GatherDataAssetData(DA);

			{
				FScopeLock Lock(&InFlightLock);
				if (InFlightPackages.Contains(Data.PackageName))
				{
					return true;
				}
			}

			UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Dispatching async audit for saved DataAsset %s"), *Data.Name);
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

	if (AssetData.IsInstanceOf(UBlueprint::StaticClass()) ||
		AssetData.IsInstanceOf(UDataTable::StaticClass()) ||
		AssetData.IsInstanceOf(UUserDefinedStruct::StaticClass()) ||
		AssetData.IsInstanceOf(UDataAsset::StaticClass()))
	{
		FBlueprintAuditor::DeleteAuditFile(FBlueprintAuditor::GetAuditOutputPath(PackageName));
	}
}

void UBlueprintAuditSubsystem::OnAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
	const FString OldPackageName = FPackageName::ObjectPathToPackageName(OldObjectPath);
	if (!OldPackageName.StartsWith(TEXT("/Game/")))
	{
		return;
	}

	if (AssetData.IsInstanceOf(UBlueprint::StaticClass()) ||
		AssetData.IsInstanceOf(UDataTable::StaticClass()) ||
		AssetData.IsInstanceOf(UUserDefinedStruct::StaticClass()) ||
		AssetData.IsInstanceOf(UDataAsset::StaticClass()))
	{
		FBlueprintAuditor::DeleteAuditFile(FBlueprintAuditor::GetAuditOutputPath(OldPackageName));
	}
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
			UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Asset registry still loading, deferring stale check..."));
			return true; // keep ticking
		}

		StaleCheckPhase = EStaleCheckPhase::BuildingList;
		return true;
	}

	case EStaleCheckPhase::BuildingList:
	{
		StaleCheckStartTime = FPlatformTime::Seconds();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		StaleCheckEntries.Reset();

		// Blueprints
		{
			TArray<FAssetData> AllBlueprints;
			AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

			for (const FAssetData& Asset : AllBlueprints)
			{
				const FString PackageName = Asset.PackageName.ToString();
				if (!PackageName.StartsWith(TEXT("/Game/")))
				{
					continue;
				}

				if (!FBlueprintAuditor::IsSupportedBlueprintClass(Asset.AssetClassPath))
				{
					UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Skipping unsupported Blueprint class %s (%s)"),
						*PackageName, *Asset.AssetClassPath.ToString());
					continue;
				}

				FStaleCheckEntry Entry;
				Entry.PackageName = PackageName;
				Entry.SourcePath = FBlueprintAuditor::GetSourceFilePath(PackageName);
				Entry.AuditPath = FBlueprintAuditor::GetAuditOutputPath(PackageName);
				Entry.AssetType = EAuditAssetType::Blueprint;
				StaleCheckEntries.Add(MoveTemp(Entry));
			}
		}

		// DataTables
		{
			TArray<FAssetData> AllDataTables;
			AssetRegistry.GetAssetsByClass(UDataTable::StaticClass()->GetClassPathName(), AllDataTables, false);

			for (const FAssetData& Asset : AllDataTables)
			{
				const FString PackageName = Asset.PackageName.ToString();
				if (!PackageName.StartsWith(TEXT("/Game/")))
				{
					continue;
				}

				FStaleCheckEntry Entry;
				Entry.PackageName = PackageName;
				Entry.SourcePath = FBlueprintAuditor::GetSourceFilePath(PackageName);
				Entry.AuditPath = FBlueprintAuditor::GetAuditOutputPath(PackageName);
				Entry.AssetType = EAuditAssetType::DataTable;
				StaleCheckEntries.Add(MoveTemp(Entry));
			}
		}

		// DataAssets
		{
			TArray<FAssetData> AllDataAssets;
			AssetRegistry.GetAssetsByClass(UDataAsset::StaticClass()->GetClassPathName(), AllDataAssets, true);

			for (const FAssetData& Asset : AllDataAssets)
			{
				const FString PackageName = Asset.PackageName.ToString();
				if (!PackageName.StartsWith(TEXT("/Game/")))
				{
					continue;
				}

				FStaleCheckEntry Entry;
				Entry.PackageName = PackageName;
				Entry.SourcePath = FBlueprintAuditor::GetSourceFilePath(PackageName);
				Entry.AuditPath = FBlueprintAuditor::GetAuditOutputPath(PackageName);
				Entry.AssetType = EAuditAssetType::DataAsset;
				StaleCheckEntries.Add(MoveTemp(Entry));
			}
		}

		// UserDefinedStructs
		{
			TArray<FAssetData> AllStructs;
			AssetRegistry.GetAssetsByClass(UUserDefinedStruct::StaticClass()->GetClassPathName(), AllStructs, false);

			for (const FAssetData& Asset : AllStructs)
			{
				const FString PackageName = Asset.PackageName.ToString();
				if (!PackageName.StartsWith(TEXT("/Game/")))
				{
					continue;
				}

				FStaleCheckEntry Entry;
				Entry.PackageName = PackageName;
				Entry.SourcePath = FBlueprintAuditor::GetSourceFilePath(PackageName);
				Entry.AuditPath = FBlueprintAuditor::GetAuditOutputPath(PackageName);
				Entry.AssetType = EAuditAssetType::UserDefinedStruct;
				StaleCheckEntries.Add(MoveTemp(Entry));
			}
		}

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Stale check Phase 1 complete: %d assets to check"), StaleCheckEntries.Num());

		// Dispatch Phase 2 to a background thread: hash comparison
		TArray<FStaleCheckEntry> EntriesCopy = StaleCheckEntries;
		Phase2Future = Async(EAsyncExecution::ThreadPool, [Entries = MoveTemp(EntriesCopy)]() -> TArray<FStaleCheckEntry>
		{
			TArray<FStaleCheckEntry> StaleResults;

			for (const FStaleCheckEntry& Entry : Entries)
			{
				if (Entry.SourcePath.IsEmpty())
				{
					continue;
				}

				const FString CurrentHash = FBlueprintAuditor::ComputeFileHash(Entry.SourcePath);
				if (CurrentHash.IsEmpty())
				{
					continue;
				}

				FString StoredHash;
				FString FileContent;
				if (FFileHelper::LoadFileToString(FileContent, *Entry.AuditPath))
				{
					const FString HashPrefix = TEXT("Hash: ");
					int32 Pos = FileContent.Find(HashPrefix);
					if (Pos != INDEX_NONE)
					{
						Pos += HashPrefix.Len();
						int32 EndPos = FileContent.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
						if (EndPos == INDEX_NONE)
						{
							EndPos = FileContent.Len();
						}
						StoredHash = FileContent.Mid(Pos, EndPos - Pos).TrimEnd();
					}
				}

				if (CurrentHash != StoredHash)
				{
					StaleResults.Add(Entry);
				}
			}

			return StaleResults;
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

		StaleEntries = Phase2Future.Get();
		StaleProcessIndex = 0;
		StaleReAuditedCount = 0;
		StaleFailedCount = 0;
		AssetsSinceGC = 0;

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Stale check Phase 2 complete: %d stale asset(s) to re-audit"), StaleEntries.Num());

		if (StaleEntries.Num() == 0)
		{
			StaleCheckPhase = EStaleCheckPhase::Done;
			return true;
		}

		StaleCheckPhase = EStaleCheckPhase::ProcessingStale;
		return true;
	}

	case EStaleCheckPhase::ProcessingStale:
	{
		const int32 BatchEnd = FMath::Min(StaleProcessIndex + StaleProcessBatchSize, StaleEntries.Num());

		for (int32 i = StaleProcessIndex; i < BatchEnd; ++i)
		{
			const FStaleCheckEntry& StaleEntry = StaleEntries[i];
			const FString& PackageName = StaleEntry.PackageName;
			const FString AssetPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);

			switch (StaleEntry.AssetType)
			{
			case EAuditAssetType::Blueprint:
			{
				UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
				if (!BP)
				{
					++StaleFailedCount;
					UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load Blueprint %s for re-audit"), *PackageName);
					break;
				}
				if (const UControlRigBlueprint* CRBP = Cast<UControlRigBlueprint>(BP))
				{
					FControlRigAuditData Data = FBlueprintAuditor::GatherControlRigData(CRBP);
					DispatchBackgroundWrite(MoveTemp(Data));
				}
				else
				{
					FBlueprintAuditData Data = FBlueprintAuditor::GatherBlueprintData(BP);
					DispatchBackgroundWrite(MoveTemp(Data));
				}
				++StaleReAuditedCount;
				break;
			}
			case EAuditAssetType::DataTable:
			{
				UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
				if (!DT)
				{
					++StaleFailedCount;
					UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load DataTable %s for re-audit"), *PackageName);
					break;
				}
				FDataTableAuditData Data = FBlueprintAuditor::GatherDataTableData(DT);
				DispatchBackgroundWrite(MoveTemp(Data));
				++StaleReAuditedCount;
				break;
			}
			case EAuditAssetType::DataAsset:
			{
				UDataAsset* DA = LoadObject<UDataAsset>(nullptr, *AssetPath);
				if (!DA)
				{
					++StaleFailedCount;
					UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load DataAsset %s for re-audit"), *PackageName);
					break;
				}
				FDataAssetAuditData Data = FBlueprintAuditor::GatherDataAssetData(DA);
				DispatchBackgroundWrite(MoveTemp(Data));
				++StaleReAuditedCount;
				break;
			}
			case EAuditAssetType::UserDefinedStruct:
			{
				UUserDefinedStruct* UDS = LoadObject<UUserDefinedStruct>(nullptr, *AssetPath);
				if (!UDS)
				{
					++StaleFailedCount;
					UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load UserDefinedStruct %s for re-audit"), *PackageName);
					break;
				}
				FUserDefinedStructAuditData Data = FBlueprintAuditor::GatherUserDefinedStructData(UDS);
				DispatchBackgroundWrite(MoveTemp(Data));
				++StaleReAuditedCount;
				break;
			}
			}

			if (++AssetsSinceGC >= GCInterval)
			{
				CollectGarbage(RF_NoFlags);
				AssetsSinceGC = 0;
			}
		}

		StaleProcessIndex = BatchEnd;

		if (StaleProcessIndex >= StaleEntries.Num())
		{
			StaleCheckPhase = EStaleCheckPhase::Done;
		}

		return true;
	}

	case EStaleCheckPhase::Done:
	{
		const double Elapsed = FPlatformTime::Seconds() - StaleCheckStartTime;
		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Stale check complete: %d scanned, %d re-audited, %d failed in %.2fs"),
			StaleCheckEntries.Num(), StaleReAuditedCount, StaleFailedCount, Elapsed);

		SweepOrphanedAuditFiles();

		// Clean up state
		StaleCheckEntries.Empty();
		StaleEntries.Empty();
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
			const FString Markdown = FBlueprintAuditor::SerializeToMarkdown(MovedData);
			FBlueprintAuditor::WriteAuditFile(Markdown, OutputPath);

			// Remove from in-flight set
			FScopeLock Lock(&InFlightLock);
			InFlightPackages.Remove(PackageName);
		});

	PendingFutures.Add(MoveTemp(Future));
}

void UBlueprintAuditSubsystem::DispatchBackgroundWrite(FDataTableAuditData&& Data)
{
	const FString PackageName = Data.PackageName;
	const FString OutputPath = Data.OutputPath;

	{
		FScopeLock Lock(&InFlightLock);
		InFlightPackages.Add(PackageName);
	}

	CleanupCompletedFutures();

	TFuture<void> Future = Async(EAsyncExecution::ThreadPool,
		[this, MovedData = MoveTemp(Data), PackageName, OutputPath]()
		{
			const FString Markdown = FBlueprintAuditor::SerializeDataTableToMarkdown(MovedData);
			FBlueprintAuditor::WriteAuditFile(Markdown, OutputPath);

			FScopeLock Lock(&InFlightLock);
			InFlightPackages.Remove(PackageName);
		});

	PendingFutures.Add(MoveTemp(Future));
}

void UBlueprintAuditSubsystem::DispatchBackgroundWrite(FDataAssetAuditData&& Data)
{
	const FString PackageName = Data.PackageName;
	const FString OutputPath = Data.OutputPath;

	{
		FScopeLock Lock(&InFlightLock);
		InFlightPackages.Add(PackageName);
	}

	CleanupCompletedFutures();

	TFuture<void> Future = Async(EAsyncExecution::ThreadPool,
		[this, MovedData = MoveTemp(Data), PackageName, OutputPath]()
		{
			const FString Markdown = FBlueprintAuditor::SerializeDataAssetToMarkdown(MovedData);
			FBlueprintAuditor::WriteAuditFile(Markdown, OutputPath);

			FScopeLock Lock(&InFlightLock);
			InFlightPackages.Remove(PackageName);
		});

	PendingFutures.Add(MoveTemp(Future));
}

void UBlueprintAuditSubsystem::DispatchBackgroundWrite(FUserDefinedStructAuditData&& Data)
{
	const FString PackageName = Data.PackageName;
	const FString OutputPath = Data.OutputPath;

	{
		FScopeLock Lock(&InFlightLock);
		InFlightPackages.Add(PackageName);
	}

	CleanupCompletedFutures();

	TFuture<void> Future = Async(EAsyncExecution::ThreadPool,
		[this, MovedData = MoveTemp(Data), PackageName, OutputPath]()
		{
			const FString Markdown = FBlueprintAuditor::SerializeUserDefinedStructToMarkdown(MovedData);
			FBlueprintAuditor::WriteAuditFile(Markdown, OutputPath);

			FScopeLock Lock(&InFlightLock);
			InFlightPackages.Remove(PackageName);
		});

	PendingFutures.Add(MoveTemp(Future));
}

void UBlueprintAuditSubsystem::DispatchBackgroundWrite(FControlRigAuditData&& Data)
{
	const FString PackageName = Data.PackageName;
	const FString OutputPath = Data.OutputPath;

	{
		FScopeLock Lock(&InFlightLock);
		InFlightPackages.Add(PackageName);
	}

	CleanupCompletedFutures();

	TFuture<void> Future = Async(EAsyncExecution::ThreadPool,
		[this, MovedData = MoveTemp(Data), PackageName, OutputPath]()
		{
			const FString Markdown = FBlueprintAuditor::SerializeControlRigToMarkdown(MovedData);
			FBlueprintAuditor::WriteAuditFile(Markdown, OutputPath);

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

void UBlueprintAuditSubsystem::SweepOrphanedAuditFilesInDir(const FString& BaseDir)
{
	TArray<FString> AuditFiles;
	IFileManager::Get().FindFilesRecursive(AuditFiles, *BaseDir, TEXT("*.md"), true, false);

	if (AuditFiles.IsEmpty())
	{
		return;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	int32 SweptCount = 0;
	for (const FString& AuditFile : AuditFiles)
	{
		FString RelPath = AuditFile;
		if (!RelPath.StartsWith(BaseDir))
		{
			continue;
		}
		RelPath.RightChopInline(BaseDir.Len());

		if (RelPath.StartsWith(TEXT("/")) || RelPath.StartsWith(TEXT("\\")))
		{
			RelPath.RightChopInline(1);
		}

		if (RelPath.EndsWith(TEXT(".md")))
		{
			RelPath.LeftChopInline(3);
		}

		RelPath.ReplaceInline(TEXT("\\"), TEXT("/"));

		const FString PackageName = TEXT("/Game/") + RelPath;

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets, true);
		if (Assets.IsEmpty())
		{
			FBlueprintAuditor::DeleteAuditFile(AuditFile);
			++SweptCount;
		}
	}

	if (SweptCount > 0)
	{
		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Swept %d orphaned audit file(s) from %s"), SweptCount, *BaseDir);
	}
}

void UBlueprintAuditSubsystem::SweepOrphanedAuditFiles()
{
	SweepOrphanedAuditFilesInDir(FBlueprintAuditor::GetAuditBaseDir());
}

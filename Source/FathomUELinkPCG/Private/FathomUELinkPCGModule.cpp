#include "FathomUELinkPCGModule.h"

#include "PCGGraphAuditor.h"
#include "PCGGraph.h"
#include "Audit/AuditExtensionRegistry.h"
#include "Audit/AuditFileUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintAuditSubsystem.h"
#include "FathomUELinkModule.h"

static constexpr int32 GCInterval = 50;

namespace
{
	/** Gather on the game thread and build a background serialize+write task for a PCG graph or graph instance. */
	TOptional<FAuditWriteTask> MakePCGWriteTask(UObject* Object)
	{
		if (const UPCGGraph* Graph = Cast<UPCGGraph>(Object))
		{
			FPCGGraphAuditData Data = FPCGGraphAuditor::GatherData(Graph);

			FAuditWriteTask Task;
			Task.PackageName = Data.PackageName;
			Task.Execute = [MovedData = MoveTemp(Data)]()
			{
				const FString Markdown = FPCGGraphAuditor::SerializeToMarkdown(MovedData);
				FAuditFileUtils::WriteAuditFile(Markdown, MovedData.OutputPath);
			};
			return Task;
		}

		if (const UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(Object))
		{
			FPCGGraphInstanceAuditData Data = FPCGGraphAuditor::GatherInstanceData(Instance);

			FAuditWriteTask Task;
			Task.PackageName = Data.PackageName;
			Task.Execute = [MovedData = MoveTemp(Data)]()
			{
				const FString Markdown = FPCGGraphAuditor::SerializeInstanceToMarkdown(MovedData);
				FAuditFileUtils::WriteAuditFile(Markdown, MovedData.OutputPath);
			};
			return Task;
		}

		return {};
	}

	/** Synchronous gather + write, used by the commandlet batch path. */
	bool AuditPCGAssetSynchronously(UObject* Object)
	{
		if (const UPCGGraph* Graph = Cast<UPCGGraph>(Object))
		{
			const FPCGGraphAuditData Data = FPCGGraphAuditor::GatherData(Graph);
			return FAuditFileUtils::WriteAuditFile(FPCGGraphAuditor::SerializeToMarkdown(Data), Data.OutputPath);
		}
		if (const UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(Object))
		{
			const FPCGGraphInstanceAuditData Data = FPCGGraphAuditor::GatherInstanceData(Instance);
			return FAuditFileUtils::WriteAuditFile(FPCGGraphAuditor::SerializeInstanceToMarkdown(Data), Data.OutputPath);
		}
		return false;
	}

	void GetAllPCGAssets(IAssetRegistry& AssetRegistry, TArray<FAssetData>& OutAssets)
	{
		const UClass* PCGClasses[] = { UPCGGraph::StaticClass(), UPCGGraphInstance::StaticClass() };
		for (const UClass* PCGClass : PCGClasses)
		{
			TArray<FAssetData> ClassAssets;
			AssetRegistry.GetAssetsByClass(PCGClass->GetClassPathName(), ClassAssets, false);
			OutAssets.Append(MoveTemp(ClassAssets));
		}
	}
}

void FFathomUELinkPCGModule::StartupModule()
{
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: FathomUELinkPCG module loaded, registering PCG auditor."));

	FAuditExtensionRegistry::FExtension Ext;
	Ext.Name = TEXT("PCG");

	// --- BatchAudit: commandlet batch scan ---
	Ext.BatchAudit = [](IAssetRegistry& AssetRegistry, int32& OutSuccess, int32& OutFail, int32& OutSkip)
	{
		TArray<FAssetData> AllPCGAssets;
		GetAllPCGAssets(AssetRegistry, AllPCGAssets);

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Auditing %d PCG asset(s)..."), AllPCGAssets.Num());

		int32 AssetsSinceGC = 0;
		for (const FAssetData& Asset : AllPCGAssets)
		{
			if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
			{
				++OutSkip;
				continue;
			}

			UObject* Loaded = Asset.GetAsset();
			if (!Loaded)
			{
				++OutFail;
				UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load PCG asset %s"), *Asset.PackageName.ToString());
				continue;
			}

			if (AuditPCGAssetSynchronously(Loaded))
			{
				++OutSuccess;
			}
			else
			{
				++OutFail;
			}

			if (++AssetsSinceGC >= GCInterval)
			{
				CollectGarbage(RF_NoFlags);
				AssetsSinceGC = 0;
			}
		}
	};

	// --- TryAuditSavedObject: on-save handler ---
	Ext.TryAuditSavedObject = [](UObject* Object) -> TOptional<FAuditWriteTask>
	{
		return MakePCGWriteTask(Object);
	};

	// --- BuildStaleCheckList: add PCG entries to stale check ---
	Ext.BuildStaleCheckList = [](IAssetRegistry& AssetRegistry, TArray<FStaleCheckEntry>& OutEntries)
	{
		TArray<FAssetData> AllPCGAssets;
		GetAllPCGAssets(AssetRegistry, AllPCGAssets);

		for (const FAssetData& Asset : AllPCGAssets)
		{
			const FString PackageName = Asset.PackageName.ToString();
			if (!PackageName.StartsWith(TEXT("/Game/")))
			{
				continue;
			}

			FStaleCheckEntry Entry;
			Entry.PackageName = PackageName;
			Entry.SourcePath = FAuditFileUtils::GetSourceFilePath(PackageName);
			Entry.AuditPath = FAuditFileUtils::GetAuditOutputPath(PackageName);
			Entry.AssetType = EAuditAssetType::PCG;
			OutEntries.Add(MoveTemp(Entry));
		}
	};

	// --- ReAuditStaleEntry: re-audit a stale PCG asset ---
	Ext.ReAuditStaleEntry = [](const FStaleCheckEntry& StaleEntry) -> TOptional<FAuditWriteTask>
	{
		if (StaleEntry.AssetType != EAuditAssetType::PCG)
		{
			return {};
		}

		const FString AssetPath = StaleEntry.PackageName + TEXT(".") + FPackageName::GetShortName(StaleEntry.PackageName);
		UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Loaded)
		{
			UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load PCG asset %s for re-audit"), *StaleEntry.PackageName);
			return {};
		}

		return MakePCGWriteTask(Loaded);
	};

	// --- IsHandledAsset: asset type check for delete/rename ---
	Ext.IsHandledAsset = [](const FAssetData& AssetData) -> bool
	{
		return AssetData.IsInstanceOf(UPCGGraph::StaticClass())
			|| AssetData.IsInstanceOf(UPCGGraphInstance::StaticClass());
	};

	FAuditExtensionRegistry::Get().RegisterExtension(MoveTemp(Ext));
}

void FFathomUELinkPCGModule::ShutdownModule()
{
	FAuditExtensionRegistry::Get().UnregisterExtension(TEXT("PCG"));
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: FathomUELinkPCG module unloaded."));
}

IMPLEMENT_MODULE(FFathomUELinkPCGModule, FathomUELinkPCG)

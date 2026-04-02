#include "FathomUELinkStateTreeModule.h"

#include "StateTreeAuditor.h"
#include "StateTree.h"
#include "Audit/AuditExtensionRegistry.h"
#include "Audit/AuditFileUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintAuditSubsystem.h"
#include "FathomUELinkModule.h"

static constexpr int32 GCInterval = 50;

void FFathomUELinkStateTreeModule::StartupModule()
{
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: FathomUELinkStateTree module loaded, registering StateTree auditor."));

	FAuditExtensionRegistry::FExtension Ext;
	Ext.Name = TEXT("StateTree");

	// --- BatchAudit: commandlet batch scan ---
	Ext.BatchAudit = [](IAssetRegistry& AssetRegistry, int32& OutSuccess, int32& OutFail, int32& OutSkip)
	{
		TArray<FAssetData> AllStateTrees;
		AssetRegistry.GetAssetsByClass(UStateTree::StaticClass()->GetClassPathName(), AllStateTrees, false);

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Auditing %d StateTree(s)..."), AllStateTrees.Num());

		int32 AssetsSinceGC = 0;
		for (const FAssetData& Asset : AllStateTrees)
		{
			if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
			{
				++OutSkip;
				continue;
			}

			const UStateTree* ST = Cast<UStateTree>(Asset.GetAsset());
			if (!ST)
			{
				++OutFail;
				UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load StateTree %s"), *Asset.PackageName.ToString());
				continue;
			}

			FStateTreeAuditData Data = FStateTreeAuditor::GatherData(ST);
			const FString Markdown = FStateTreeAuditor::SerializeToMarkdown(Data);
			if (FAuditFileUtils::WriteAuditFile(Markdown, Data.OutputPath))
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
		const UStateTree* ST = Cast<UStateTree>(Object);
		if (!ST)
		{
			return {};
		}

		// Gather on game thread
		FStateTreeAuditData Data = FStateTreeAuditor::GatherData(ST);

		FAuditWriteTask Task;
		Task.PackageName = Data.PackageName;
		Task.Execute = [MovedData = MoveTemp(Data)]()
		{
			const FString Markdown = FStateTreeAuditor::SerializeToMarkdown(MovedData);
			FAuditFileUtils::WriteAuditFile(Markdown, MovedData.OutputPath);
		};

		return Task;
	};

	// --- BuildStaleCheckList: add StateTree entries to stale check ---
	Ext.BuildStaleCheckList = [](IAssetRegistry& AssetRegistry, TArray<FStaleCheckEntry>& OutEntries)
	{
		TArray<FAssetData> AllStateTrees;
		AssetRegistry.GetAssetsByClass(UStateTree::StaticClass()->GetClassPathName(), AllStateTrees, false);

		for (const FAssetData& Asset : AllStateTrees)
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
			Entry.AssetType = EAuditAssetType::StateTree;
			OutEntries.Add(MoveTemp(Entry));
		}
	};

	// --- ReAuditStaleEntry: re-audit a stale StateTree ---
	Ext.ReAuditStaleEntry = [](const FStaleCheckEntry& StaleEntry) -> TOptional<FAuditWriteTask>
	{
		if (StaleEntry.AssetType != EAuditAssetType::StateTree)
		{
			return {};
		}

		const FString AssetPath = StaleEntry.PackageName + TEXT(".") + FPackageName::GetShortName(StaleEntry.PackageName);
		UStateTree* ST = LoadObject<UStateTree>(nullptr, *AssetPath);
		if (!ST)
		{
			UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to load StateTree %s for re-audit"), *StaleEntry.PackageName);
			return {};
		}

		FStateTreeAuditData Data = FStateTreeAuditor::GatherData(ST);

		FAuditWriteTask Task;
		Task.PackageName = Data.PackageName;
		Task.Execute = [MovedData = MoveTemp(Data)]()
		{
			const FString Markdown = FStateTreeAuditor::SerializeToMarkdown(MovedData);
			FAuditFileUtils::WriteAuditFile(Markdown, MovedData.OutputPath);
		};

		return Task;
	};

	// --- IsHandledAsset: asset type check for delete/rename ---
	Ext.IsHandledAsset = [](const FAssetData& AssetData) -> bool
	{
		return AssetData.IsInstanceOf(UStateTree::StaticClass());
	};

	FAuditExtensionRegistry::Get().RegisterExtension(MoveTemp(Ext));
}

void FFathomUELinkStateTreeModule::ShutdownModule()
{
	FAuditExtensionRegistry::Get().UnregisterExtension(TEXT("StateTree"));
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: FathomUELinkStateTree module unloaded."));
}

IMPLEMENT_MODULE(FFathomUELinkStateTreeModule, FathomUELinkStateTree)

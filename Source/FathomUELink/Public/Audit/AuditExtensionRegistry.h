#pragma once

#include "CoreMinimal.h"

struct FStaleCheckEntry;
class IAssetRegistry;

/**
 * A background write task produced by an extension auditor.
 *
 * The extension gathers data on the game thread and captures it into the
 * Execute closure, which the subsystem dispatches to the thread pool for
 * serialization and file write.
 */
struct FATHOMUELINK_API FAuditWriteTask
{
	/** Package name for in-flight dedup (e.g. "/Game/AI/ST_Enemy"). */
	FString PackageName;

	/** Closure that serializes gathered data and writes the audit file. Runs on a worker thread. */
	TFunction<void()> Execute;
};

/**
 * Registry for optional auditor extensions.
 *
 * Optional modules (e.g. FathomUELinkStateTree) register callbacks during
 * StartupModule(). The commandlet, subsystem, and stale checker invoke these
 * callbacks at the appropriate points in their pipelines.
 */
struct FATHOMUELINK_API FAuditExtensionRegistry
{
	/** A single extension registration. */
	struct FExtension
	{
		FName Name;

		/**
		 * Commandlet batch: scan asset registry, gather, serialize, write.
		 * Accumulate into the provided counters.
		 */
		TFunction<void(IAssetRegistry&, int32& OutSuccess, int32& OutFail, int32& OutSkip)> BatchAudit;

		/**
		 * On-save: attempt to handle a saved object. Gather data on the game
		 * thread and return an FAuditWriteTask whose Execute closure captures
		 * the gathered POD for background serialization+write.
		 * Return empty if the object is not handled by this extension.
		 */
		TFunction<TOptional<FAuditWriteTask>(UObject*)> TryAuditSavedObject;

		/**
		 * Stale-check list builder: append FStaleCheckEntry items for assets
		 * this extension handles.
		 */
		TFunction<void(IAssetRegistry&, TArray<FStaleCheckEntry>&)> BuildStaleCheckList;

		/**
		 * Stale re-audit: load and re-audit a single stale entry.
		 * Return empty if this entry's asset type is not handled.
		 */
		TFunction<TOptional<FAuditWriteTask>(const FStaleCheckEntry&)> ReAuditStaleEntry;

		/**
		 * Asset type check for delete/rename cleanup.
		 * Return true if the given asset data is of a type this extension handles.
		 */
		TFunction<bool(const FAssetData&)> IsHandledAsset;
	};

	static FAuditExtensionRegistry& Get();

	void RegisterExtension(FExtension&& Extension);
	void UnregisterExtension(FName Name);

	const TArray<FExtension>& GetExtensions() const { return Extensions; }

private:
	TArray<FExtension> Extensions;
};

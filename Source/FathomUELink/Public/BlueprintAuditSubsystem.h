#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "EditorSubsystem.h"
#include "BlueprintAuditor.h"
#include "Audit/AuditExtensionRegistry.h"
#include "BlueprintAuditSubsystem.generated.h"

/** State machine phases for the startup stale check. */
enum class EStaleCheckPhase : uint8
{
	Idle,
	WaitingForRegistry,
	BuildingList,
	BackgroundHash,
	ProcessingStale,
	ProcessingStaleWithProgress,
	Done
};

/** The type of asset being audited. */
enum class EAuditAssetType : uint8
{
	Blueprint,
	DataTable,
	DataAsset,
	UserDefinedStruct,
	ControlRig,
	Material,
	BehaviorTree,
	StateTree,
	PCG
};

/** Per-entry data collected in Phase 1, consumed in Phase 2/3. */
struct FStaleCheckEntry
{
	FString PackageName;
	FString SourcePath;
	FString AuditPath;
	EAuditAssetType AssetType = EAuditAssetType::Blueprint;
};

/**
 * Editor subsystem that automatically audits Blueprint assets on save.
 * Hooks into UPackage::PackageSavedWithContextEvent and writes a per-file
 * Markdown audit to Saved/Fathom/Audit/Blueprints/, mirroring the Content directory layout.
 *
 * On startup, runs a three-phase stale check that offloads hashing and I/O
 * to background threads and chunks game-thread work across ticks to avoid
 * freezing the editor UI.
 */
UCLASS()
class FATHOMUELINK_API UBlueprintAuditSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	//~ UEditorSubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext ObjectSaveContext);

	/** Delete the audit file when a Blueprint asset is removed from the project. */
	void OnAssetRemoved(const FAssetData& AssetData);

	/** Delete the old-path audit file when a Blueprint asset is renamed or moved. */
	void OnAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath);

	/** Ticker callback: drives the stale check state machine. */
	bool OnStaleCheckTick(float DeltaTime);

	/**
	 * Re-audit a single stale entry on the game thread: LoadObject + Gather + dispatch
	 * background serialize/write. Shared by the small-batch ticker path and the
	 * FScopedSlowTask bulk path.
	 */
	void ProcessSingleStaleEntry(const FStaleCheckEntry& Entry);

	/**
	 * Run the entire StaleEntries set synchronously inside an FScopedSlowTask,
	 * showing a cancelable progress dialog. Used when the stale count is large
	 * (e.g. schema bump or first run): one big visible operation beats minutes
	 * of invisible per-frame stutter. Must be called on the game thread.
	 */
	void RunStaleProcessingWithProgressDialog();

	/** Walk the audit directory and delete audit files whose source .uasset no longer exists. */
	void SweepOrphanedAuditFiles();

	/** Walk a single audit directory and delete files whose source .uasset no longer exists. */
	void SweepOrphanedAuditFilesInDir(const FString& BaseDir);

	/**
	 * Dispatch serialization + file write to a background thread.
	 * Shared by OnPackageSaved and stale check Phase 3.
	 * Takes ownership of Data by move.
	 */
	void DispatchBackgroundWrite(FBlueprintAuditData&& Data);
	void DispatchBackgroundWrite(FDataTableAuditData&& Data);
	void DispatchBackgroundWrite(FDataAssetAuditData&& Data);
	void DispatchBackgroundWrite(FUserDefinedStructAuditData&& Data);
	void DispatchBackgroundWrite(FControlRigAuditData&& Data);
	void DispatchBackgroundWrite(FMaterialAuditData&& Data);
	void DispatchBackgroundWrite(FBehaviorTreeAuditData&& Data);

	/** Dispatch a generic write task from an extension auditor. */
	void DispatchBackgroundWriteTask(FAuditWriteTask&& Task);

	/** Remove completed futures from PendingFutures to prevent unbounded growth. */
	void CleanupCompletedFutures();

	// --- Ticker ---
	FTSTicker::FDelegateHandle StaleCheckTickerHandle;

	// --- Stale check state machine ---
	EStaleCheckPhase StaleCheckPhase = EStaleCheckPhase::WaitingForRegistry;
	TArray<FStaleCheckEntry> StaleCheckEntries;
	TArray<FStaleCheckEntry> StaleEntries;
	int32 StaleProcessIndex = 0;
	int32 StaleReAuditedCount = 0;
	int32 StaleFailedCount = 0;
	int32 AssetsSinceGC = 0;
	int32 TickFrameCounter = 0;
	double StaleCheckStartTime = 0.0;

	/** Phase 2: background future that computes hashes and returns stale entries. */
	TFuture<TArray<FStaleCheckEntry>> Phase2Future;

	// --- Background write tracking ---
	TArray<TFuture<void>> PendingFutures;

	// --- In-flight dedup ---
	FCriticalSection InFlightLock;
	TSet<FString> InFlightPackages;

	// --- Constants ---
	/**
	 * Small-batch path: process one stale entry every Nth ticker callback.
	 * LoadObject<UBlueprint> is the indivisible hitch unit (~100-500ms per heavy BP);
	 * spacing rather than batching keeps individual frame freezes short and gives
	 * the editor breathing room between hitches.
	 */
	static constexpr int32 FramesPerStaleEntry = 3;

	/**
	 * If the stale count meets or exceeds this, switch from invisible ticker pacing
	 * to a visible FScopedSlowTask with cancel button. Below this, the per-frame
	 * stutter is brief enough to remain invisible to most users.
	 */
	static constexpr int32 SlowTaskThreshold = 25;

	static constexpr int32 GCInterval = 50;
};

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "EditorSubsystem.h"
#include "BlueprintAuditor.h"
#include "BlueprintAuditSubsystem.generated.h"

/** State machine phases for the startup stale check. */
enum class EStaleCheckPhase : uint8
{
	Idle,
	WaitingForRegistry,
	BuildingList,
	BackgroundHash,
	ProcessingStale,
	Done
};

/** Per-entry data collected in Phase 1, consumed in Phase 2/3. */
struct FStaleCheckEntry
{
	FString PackageName;
	FString SourcePath;
	FString AuditPath;
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

	/** Walk the audit directory and delete audit files whose source .uasset no longer exists. */
	void SweepOrphanedAuditFiles();

	/**
	 * Dispatch serialization + file write to a background thread.
	 * Shared by OnPackageSaved and stale check Phase 3.
	 * Takes ownership of Data by move.
	 */
	void DispatchBackgroundWrite(FBlueprintAuditData&& Data);

	/** Remove completed futures from PendingFutures to prevent unbounded growth. */
	void CleanupCompletedFutures();

	// --- Ticker ---
	FTSTicker::FDelegateHandle StaleCheckTickerHandle;

	// --- Stale check state machine ---
	EStaleCheckPhase StaleCheckPhase = EStaleCheckPhase::WaitingForRegistry;
	TArray<FStaleCheckEntry> StaleCheckEntries;
	TArray<FString> StalePackageNames;
	int32 StaleProcessIndex = 0;
	int32 StaleReAuditedCount = 0;
	int32 StaleFailedCount = 0;
	int32 AssetsSinceGC = 0;
	double StaleCheckStartTime = 0.0;

	/** Phase 2: background future that computes hashes and returns stale package names. */
	TFuture<TArray<FString>> Phase2Future;

	// --- Background write tracking ---
	TArray<TFuture<void>> PendingFutures;

	// --- In-flight dedup ---
	FCriticalSection InFlightLock;
	TSet<FString> InFlightPackages;

	// --- Constants ---
	static constexpr int32 StaleProcessBatchSize = 5;
	static constexpr int32 GCInterval = 50;
};

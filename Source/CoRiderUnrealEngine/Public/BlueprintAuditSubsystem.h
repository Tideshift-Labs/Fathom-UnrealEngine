#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "EditorSubsystem.h"
#include "BlueprintAuditSubsystem.generated.h"

/**
 * Editor subsystem that automatically audits Blueprint assets on save.
 * Hooks into UPackage::PackageSavedWithContextEvent and writes a per-file
 * JSON audit to Saved/Audit/Blueprints/, mirroring the Content directory layout.
 *
 * On startup, runs a deferred stale-check: compares each Blueprint's .uasset
 * MD5 hash against the stored SourceFileHash in its audit JSON. Any stale or
 * missing entries are re-audited automatically.
 */
UCLASS()
class CORIDERUNREALENGINE_API UBlueprintAuditSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	//~ UEditorSubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext ObjectSaveContext);

	/** Delete the audit JSON when a Blueprint asset is removed from the project. */
	void OnAssetRemoved(const FAssetData& AssetData);

	/** Delete the old-path audit JSON when a Blueprint asset is renamed or moved. */
	void OnAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath);

	/** Ticker callback: waits for asset registry, then runs the stale check once. */
	bool OnStaleCheckTick(float DeltaTime);

	/** Iterate all project Blueprints and re-audit any whose .uasset hash differs from the stored JSON hash. */
	void AuditStaleBlueprints();

	/** Walk the audit directory and delete JSON files whose source .uasset no longer exists. */
	void SweepOrphanedAuditFiles();

	FTSTicker::FDelegateHandle StaleCheckTickerHandle;
};

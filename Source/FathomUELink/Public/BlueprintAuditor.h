#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"
#include "Audit/AuditFileUtils.h"
#include "Audit/BlueprintGraphAuditor.h"
#include "Audit/DataTableAuditor.h"
#include "Audit/DataAssetAuditor.h"
#include "Audit/UserDefinedStructAuditor.h"
#include "Audit/ControlRigAuditor.h"

class UBlueprint;
class UControlRigBlueprint;
class UDataAsset;
class UDataTable;
class UEdGraph;
class UUserDefinedStruct;
struct FEdGraphPinType;
struct FTopLevelAssetPath;

/**
 * Shared utility for auditing Blueprint assets.
 * Used by both BlueprintAuditCommandlet (batch) and BlueprintAuditSubsystem (on-save).
 *
 * This is a thin facade that delegates to domain-specific auditors.
 * New code should prefer using the domain auditors directly.
 */
struct FATHOMUELINK_API FBlueprintAuditor
{
	/** Bump when the audit format changes to invalidate all cached audit files. */
	static constexpr int32 AuditSchemaVersion = FAuditFileUtils::AuditSchemaVersion;

	// --- Game-thread gather (reads UObject pointers, populates POD structs) ---

	/** Gather all audit data from a Blueprint into a POD struct. Must be called on the game thread. */
	static FBlueprintAuditData GatherBlueprintData(const UBlueprint* BP);

	/** Gather audit data from a single graph. Must be called on the game thread. */
	static FGraphAuditData GatherGraphData(const UEdGraph* Graph);

	/** Gather audit data from a widget and its children. Must be called on the game thread. */
	static FWidgetAuditData GatherWidgetData(class UWidget* Widget);

	// --- Thread-safe serialization (POD to Markdown, no UObject access) ---

	/** Serialize gathered Blueprint data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FBlueprintAuditData& Data);

	/** Serialize gathered graph data to Markdown. Safe on any thread. */
	static FString SerializeGraphToMarkdown(const FGraphAuditData& Data, const FString& Prefix);

	/** Serialize gathered widget data to a Markdown indented list. Safe on any thread. */
	static FString SerializeWidgetToMarkdown(const FWidgetAuditData& Data, int32 Indent = 0);

	// --- DataTable gather + serialize ---

	/** Gather all audit data from a DataTable into a POD struct. Must be called on the game thread. */
	static FDataTableAuditData GatherDataTableData(const UDataTable* DataTable);

	/** Serialize gathered DataTable data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeDataTableToMarkdown(const FDataTableAuditData& Data);

	// --- DataAsset gather + serialize ---

	/** Gather all audit data from a DataAsset into a POD struct. Must be called on the game thread. */
	static FDataAssetAuditData GatherDataAssetData(const UDataAsset* Asset);

	/** Serialize gathered DataAsset data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeDataAssetToMarkdown(const FDataAssetAuditData& Data);

	// --- UserDefinedStruct gather + serialize ---

	/** Gather all audit data from a UserDefinedStruct into a POD struct. Must be called on the game thread. */
	static FUserDefinedStructAuditData GatherUserDefinedStructData(const UUserDefinedStruct* Struct);

	/** Serialize gathered UserDefinedStruct data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeUserDefinedStructToMarkdown(const FUserDefinedStructAuditData& Data);

	// --- ControlRig gather + serialize ---

	/** Gather all audit data from a ControlRig Blueprint into a POD struct. Must be called on the game thread. */
	static FControlRigAuditData GatherControlRigData(const UControlRigBlueprint* CRBP);

	/** Serialize gathered ControlRig data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeControlRigToMarkdown(const FControlRigAuditData& Data);

	// --- Legacy synchronous API (used by Commandlet and as a convenience wrapper) ---

	/** Produce a Markdown string summarizing the given Blueprint. Equivalent to SerializeToMarkdown(GatherBlueprintData(BP)). */
	static FString AuditBlueprint(const UBlueprint* BP);

	/** Produce a Markdown string summarizing a single graph. */
	static FString AuditGraph(const UEdGraph* Graph);

	/** Produce a Markdown string summarizing a single widget and its children. */
	static FString AuditWidget(class UWidget* Widget);

	/** Human-readable type string for a Blueprint variable pin type. */
	static FString GetVariableTypeString(const FEdGraphPinType& PinType);

	/** Return the base directory for all audit files: <ProjectDir>/Saved/Fathom/Audit/v<N>/ */
	static FString GetAuditBaseDir();

	/**
	 * Compute the on-disk output path for an asset's audit file.
	 * e.g. /Game/UI/Widgets/WBP_Foo  ->  <ProjectDir>/Saved/Fathom/Audit/v<N>/UI/Widgets/WBP_Foo.md
	 */
	static FString GetAuditOutputPath(const UBlueprint* BP);
	static FString GetAuditOutputPath(const FString& PackageName);

	/** Delete an audit file. Returns true if the file was deleted or did not exist. */
	static bool DeleteAuditFile(const FString& FilePath);

	/**
	 * Convert a package name (e.g. /Game/UI/WBP_Foo) to its .uasset file path on disk.
	 */
	static FString GetSourceFilePath(const FString& PackageName);

	/** Compute an MD5 hash of the file at the given path. Returns empty string on failure. */
	static FString ComputeFileHash(const FString& FilePath);

	/** Write audit content to disk. Returns true on success. */
	static bool WriteAuditFile(const FString& Content, const FString& OutputPath);

	/**
	 * Returns true if the given Blueprint native class is safe to load and audit.
	 * Some Blueprint subclasses (ControlRig, RigVM) crash during LoadObject because
	 * their subsystems expect a specific loading context we cannot provide.
	 */
	static bool IsSupportedBlueprintClass(const FTopLevelAssetPath& ClassPath);
};

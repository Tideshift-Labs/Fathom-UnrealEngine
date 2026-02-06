#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
struct FEdGraphPinType;

DECLARE_LOG_CATEGORY_EXTERN(LogBlueprintAudit, Log, All);

/**
 * Shared utility for auditing Blueprint assets.
 * Used by both BlueprintAuditCommandlet (batch) and BlueprintAuditSubsystem (on-save).
 */
struct CORIDERUNREALENGINE_API FBlueprintAuditor
{
	/** Produce a JSON object summarizing the given Blueprint. */
	static TSharedPtr<FJsonObject> AuditBlueprint(const UBlueprint* BP);

	/** Produce a JSON object summarizing a single graph. */
	static TSharedPtr<FJsonObject> AuditGraph(const UEdGraph* Graph);

	/** Produce a JSON object summarizing a single widget and its children. */
	static TSharedPtr<FJsonObject> AuditWidget(class UWidget* Widget);

	/** Human-readable type string for a Blueprint variable pin type. */
	static FString GetVariableTypeString(const FEdGraphPinType& PinType);

	/**
	 * Compute the on-disk output path for a Blueprint's audit JSON.
	 * e.g. /Game/UI/Widgets/WBP_Foo  ->  <ProjectDir>/Saved/Audit/Blueprints/UI/Widgets/WBP_Foo.json
	 */
	static FString GetAuditOutputPath(const UBlueprint* BP);
	static FString GetAuditOutputPath(const FString& PackageName);

	/**
	 * Convert a package name (e.g. /Game/UI/WBP_Foo) to its .uasset file path on disk.
	 */
	static FString GetSourceFilePath(const FString& PackageName);

	/** Compute an MD5 hash of the file at the given path. Returns empty string on failure. */
	static FString ComputeFileHash(const FString& FilePath);

	/** Serialize a JSON object and write it to disk. Returns true on success. */
	static bool WriteAuditJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& OutputPath);
};

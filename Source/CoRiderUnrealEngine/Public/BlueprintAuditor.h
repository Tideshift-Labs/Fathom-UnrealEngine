#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
struct FEdGraphPinType;

DECLARE_LOG_CATEGORY_EXTERN(LogCoRider, Log, All);

// --- POD audit data structs (no UObject pointers, safe to move across threads) ---

struct FVariableAuditData
{
	FString Name;
	FString Type;
	FString Category;
	bool bInstanceEditable = false;
	bool bReplicated = false;
};

struct FPropertyOverrideData
{
	FString Name;
	FString Value;
};

struct FComponentAuditData
{
	FString Name;
	FString Class;
};

struct FTimelineAuditData
{
	FString Name;
	float Length = 0.f;
	bool bLooping = false;
	bool bAutoPlay = false;
	int32 FloatTrackCount = 0;
	int32 VectorTrackCount = 0;
	int32 LinearColorTrackCount = 0;
	int32 EventTrackCount = 0;
};

struct FDefaultInputData
{
	FString Name;
	FString Value;
};

struct FGraphParamData
{
	FString Name;
	FString Type;
};

struct FNodeAuditData
{
	int32 Id = 0;
	FString Type;    // "FunctionEntry", "FunctionResult", "Event", "CustomEvent",
	                 // "CallFunction", "Branch", "Sequence", "VariableGet",
	                 // "VariableSet", "MacroInstance", "Timeline", "Other"
	FString Name;
	FString Target;  // owning class for CallFunction (empty otherwise)
	bool bIsNative = false;
	bool bPure = false;
	bool bLatent = false;
	TArray<FDefaultInputData> DefaultInputs;
};

struct FExecEdge
{
	int32 SourceNodeId = 0;
	FString SourcePinName;  // "Then", "True", "False", "Completed", etc.
	int32 TargetNodeId = 0;
};

struct FDataEdge
{
	int32 SourceNodeId = 0;
	FString SourcePinName;  // "ReturnValue", etc.
	int32 TargetNodeId = 0;
	FString TargetPinName;  // "Condition", "InString", etc.
};

struct FGraphAuditData
{
	FString Name;

	// Function/macro signature (populated for function and macro graphs)
	TArray<FGraphParamData> Inputs;
	TArray<FGraphParamData> Outputs;

	// Graph topology
	TArray<FNodeAuditData> Nodes;
	TArray<FExecEdge> ExecFlows;
	TArray<FDataEdge> DataFlows;
};

struct FWidgetAuditData
{
	FString Name;
	FString Class;
	bool bIsVariable = false;
	TArray<FWidgetAuditData> Children;
};

struct FBlueprintAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString ParentClass;
	FString BlueprintType;
	FString SourceFilePath;
	FString OutputPath;

	TArray<FVariableAuditData> Variables;
	TArray<FPropertyOverrideData> PropertyOverrides;
	TArray<FString> Interfaces;
	TArray<FComponentAuditData> Components;
	TArray<FTimelineAuditData> Timelines;
	TArray<FGraphAuditData> EventGraphs;
	TArray<FGraphAuditData> FunctionGraphs;
	TArray<FGraphAuditData> MacroGraphs;

	/** Set if this is a Widget Blueprint. */
	TOptional<FWidgetAuditData> WidgetTree;
};

/**
 * Shared utility for auditing Blueprint assets.
 * Used by both BlueprintAuditCommandlet (batch) and BlueprintAuditSubsystem (on-save).
 */
struct CORIDERUNREALENGINE_API FBlueprintAuditor
{
	/** Bump when the JSON schema changes to invalidate all cached audit files. */
	static constexpr int32 AuditSchemaVersion = 3;

	// --- Game-thread gather (reads UObject pointers, populates POD structs) ---

	/** Gather all audit data from a Blueprint into a POD struct. Must be called on the game thread. */
	static FBlueprintAuditData GatherBlueprintData(const UBlueprint* BP);

	/** Gather audit data from a single graph. Must be called on the game thread. */
	static FGraphAuditData GatherGraphData(const UEdGraph* Graph);

	/** Gather audit data from a widget and its children. Must be called on the game thread. */
	static FWidgetAuditData GatherWidgetData(class UWidget* Widget);

	// --- Thread-safe serialization (POD to JSON, no UObject access) ---

	/** Serialize gathered Blueprint data to JSON. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static TSharedPtr<FJsonObject> SerializeToJson(const FBlueprintAuditData& Data);

	/** Serialize gathered graph data to JSON. Safe on any thread. */
	static TSharedPtr<FJsonObject> SerializeGraphToJson(const FGraphAuditData& Data);

	/** Serialize gathered widget data to JSON. Safe on any thread. */
	static TSharedPtr<FJsonObject> SerializeWidgetToJson(const FWidgetAuditData& Data);

	// --- Legacy synchronous API (used by Commandlet and as a convenience wrapper) ---

	/** Produce a JSON object summarizing the given Blueprint. Equivalent to SerializeToJson(GatherBlueprintData(BP)). */
	static TSharedPtr<FJsonObject> AuditBlueprint(const UBlueprint* BP);

	/** Produce a JSON object summarizing a single graph. */
	static TSharedPtr<FJsonObject> AuditGraph(const UEdGraph* Graph);

	/** Produce a JSON object summarizing a single widget and its children. */
	static TSharedPtr<FJsonObject> AuditWidget(class UWidget* Widget);

	/** Human-readable type string for a Blueprint variable pin type. */
	static FString GetVariableTypeString(const FEdGraphPinType& PinType);

	/** Return the base directory for all audit JSON files: <ProjectDir>/Saved/Audit/v<N>/Blueprints */
	static FString GetAuditBaseDir();

	/**
	 * Compute the on-disk output path for a Blueprint's audit JSON.
	 * e.g. /Game/UI/Widgets/WBP_Foo  ->  <ProjectDir>/Saved/Audit/v<N>/Blueprints/UI/Widgets/WBP_Foo.json
	 */
	static FString GetAuditOutputPath(const UBlueprint* BP);
	static FString GetAuditOutputPath(const FString& PackageName);

	/** Delete an audit JSON file. Returns true if the file was deleted or did not exist. */
	static bool DeleteAuditJson(const FString& JsonPath);

	/**
	 * Convert a package name (e.g. /Game/UI/WBP_Foo) to its .uasset file path on disk.
	 */
	static FString GetSourceFilePath(const FString& PackageName);

	/** Compute an MD5 hash of the file at the given path. Returns empty string on failure. */
	static FString ComputeFileHash(const FString& FilePath);

	/** Serialize a JSON object and write it to disk. Returns true on success. */
	static bool WriteAuditJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& OutputPath);
};

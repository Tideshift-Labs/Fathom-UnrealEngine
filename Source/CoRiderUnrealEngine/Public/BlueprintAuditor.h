#pragma once

#include "CoreMinimal.h"

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
	FString SlotName;  // Non-empty when this widget is content placed in a named slot
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
	/** Bump when the audit format changes to invalidate all cached audit files. */
	static constexpr int32 AuditSchemaVersion = 4;

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

	// --- Legacy synchronous API (used by Commandlet and as a convenience wrapper) ---

	/** Produce a Markdown string summarizing the given Blueprint. Equivalent to SerializeToMarkdown(GatherBlueprintData(BP)). */
	static FString AuditBlueprint(const UBlueprint* BP);

	/** Produce a Markdown string summarizing a single graph. */
	static FString AuditGraph(const UEdGraph* Graph);

	/** Produce a Markdown string summarizing a single widget and its children. */
	static FString AuditWidget(class UWidget* Widget);

	/** Human-readable type string for a Blueprint variable pin type. */
	static FString GetVariableTypeString(const FEdGraphPinType& PinType);

	/** Return the base directory for all audit files: <ProjectDir>/Saved/Audit/v<N>/Blueprints */
	static FString GetAuditBaseDir();

	/**
	 * Compute the on-disk output path for a Blueprint's audit file.
	 * e.g. /Game/UI/Widgets/WBP_Foo  ->  <ProjectDir>/Saved/Audit/v<N>/Blueprints/UI/Widgets/WBP_Foo.md
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
};

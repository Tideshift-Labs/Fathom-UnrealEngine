#pragma once

#include "CoreMinimal.h"

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
	                 // "VariableSet", "MacroInstance", "Timeline",
	                 // "CollapsedNode", "Tunnel", "Other"
	FString Name;
	FString Target;  // owning class for CallFunction (empty otherwise)
	bool bIsNative = false;
	bool bPure = false;
	bool bLatent = false;
	TArray<FDefaultInputData> DefaultInputs;
	FString CompilerMessage;  // e.g. "Error: Accessed None trying to read property Health"
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

	// Collapsed sub-graphs (UK2Node_Composite bound graphs, can nest recursively)
	TArray<FGraphAuditData> SubGraphs;
};

struct FWidgetAuditData
{
	FString Name;
	FString Class;
	bool bIsVariable = false;
	FString SlotName;  // Non-empty when this widget is content placed in a named slot
	TArray<FWidgetAuditData> Children;
};

// --- DataTable audit data ---

struct FDataTableColumnDef
{
	FString Name;
	FString Type;
};

struct FDataTableRowData
{
	FString RowName;
	TArray<FString> Values;
};

struct FDataTableAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString RowStructName;
	FString RowStructPath;
	FString SourceFilePath;
	FString OutputPath;
	TArray<FDataTableColumnDef> Columns;
	TArray<FDataTableRowData> Rows;
};

// --- DataAsset audit data ---

struct FDataAssetAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString NativeClass;
	FString NativeClassPath;
	FString SourceFilePath;
	FString OutputPath;
	TArray<FPropertyOverrideData> Properties;
};

// --- UserDefinedStruct audit data ---

struct FStructFieldDef
{
	FString Name;
	FString Type;
	FString DefaultValue;
};

struct FUserDefinedStructAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString SourceFilePath;
	FString OutputPath;
	TArray<FStructFieldDef> Fields;
};

// --- ControlRig audit data ---

struct FRigVMPinAuditData
{
	FString Name;
	FString CPPType;       // "float", "FVector", "FRigElementKey"
	FString Direction;     // "Input", "Output", "IO", "Hidden"
	FString DefaultValue;
};

struct FRigVMNodeAuditData
{
	int32 Id = 0;
	FString Type;          // "Unit", "Variable", "FunctionRef", "FunctionEntry",
	                       // "FunctionReturn", "Collapse", "Other"
	FString Name;
	FString StructPath;    // for Unit nodes: e.g. "FRigUnit_SetBoneTransform"
	FString MethodName;    // for Unit nodes: e.g. "Execute"
	bool bIsMutable = false;
	bool bIsPure = false;
	bool bIsEvent = false;
	TArray<FRigVMPinAuditData> Pins;
};

struct FRigVMEdgeAuditData
{
	int32 SourceNodeId = 0;
	FString SourcePinPath;
	int32 TargetNodeId = 0;
	FString TargetPinPath;
};

struct FRigVMGraphAuditData
{
	FString Name;
	bool bIsRootGraph = false;
	TArray<FGraphParamData> Inputs;
	TArray<FGraphParamData> Outputs;
	TArray<FRigVMNodeAuditData> Nodes;
	TArray<FRigVMEdgeAuditData> Edges;
};

struct FControlRigAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString ParentClass;
	FString SourceFilePath;
	FString OutputPath;

	TArray<FVariableAuditData> Variables;
	TArray<FRigVMGraphAuditData> Graphs;
};

struct FBlueprintAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString ParentClass;
	FString BlueprintType;
	FString CompileStatus;  // e.g. "Error", "UpToDate", "Dirty"
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

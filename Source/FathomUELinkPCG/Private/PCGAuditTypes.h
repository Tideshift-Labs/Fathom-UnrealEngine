#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

// --- PCG audit data structs (no UObject pointers, safe to move across threads) ---

struct FPCGPinAuditData
{
	FString Label;
	FString Direction;     // "Input" / "Output"
	FString AllowedTypes;  // human-readable allowed data types, e.g. "Point", "Spatial", "Any"
	bool bAllowMultipleData = true;
};

struct FPCGNodeAuditData
{
	int32 Id = 0;
	FString Type;           // EPCGSettingsType name, e.g. "Sampler", "Spawner"; "Generic" fallback
	FString Title;          // node display title
	FString SettingsClass;  // short settings class name, e.g. "PCGSurfaceSamplerSettings"
	bool bIsSubgraph = false;
	FString SubgraphPath;          // referenced UPCGGraph asset path (subgraph nodes only)
	FString SubgraphInstancePath;  // set when the reference goes through a UPCGGraphInstance asset
	TArray<FPCGPinAuditData> Pins;
	TArray<FPropertyOverrideData> Settings;  // overridable param values
};

struct FPCGEdgeAuditData
{
	int32 SourceNodeId = 0;
	FString SourcePinLabel;
	int32 TargetNodeId = 0;
	FString TargetPinLabel;
};

struct FPCGGraphParamData
{
	FString Name;
	FString Type;
	FString Value;
	bool bOverridden = false;  // graph instances only
};

struct FPCGGraphAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString SourceFilePath;
	FString OutputPath;

	TArray<FPCGGraphParamData> Parameters;  // graph-level user parameters
	TArray<FPCGNodeAuditData> Nodes;
	TArray<FPCGEdgeAuditData> Edges;
};

struct FPCGGraphInstanceAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString SourceFilePath;
	FString OutputPath;

	FString ParentGraphPath;  // directly referenced graph or instance asset
	FString BaseGraphPath;    // resolved concrete UPCGGraph when the parent is itself an instance
	TArray<FPCGGraphParamData> Parameters;
};

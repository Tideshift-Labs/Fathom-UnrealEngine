#pragma once

#include "CoreMinimal.h"
#include "PCGAuditTypes.h"

class UPCGGraph;
class UPCGGraphInstance;

/**
 * Auditor for PCG graph and graph instance assets.
 * Extracts graph-level user parameters, nodes with their settings values,
 * and edges traced through reroute nodes to real endpoints.
 * Subgraphs are recorded as asset path references, not inlined; every
 * project subgraph gets its own audit file from the same extension.
 */
struct FPCGGraphAuditor
{
	/** Gather all audit data from a PCG graph into a POD struct. Must be called on the game thread. */
	static FPCGGraphAuditData GatherData(const UPCGGraph* Graph);

	/** Gather audit data from a PCG graph instance asset. Must be called on the game thread. */
	static FPCGGraphInstanceAuditData GatherInstanceData(const UPCGGraphInstance* Instance);

	/** Serialize gathered graph data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FPCGGraphAuditData& Data);

	/** Serialize gathered graph instance data to Markdown. Safe on any thread. */
	static FString SerializeInstanceToMarkdown(const FPCGGraphInstanceAuditData& Data);
};

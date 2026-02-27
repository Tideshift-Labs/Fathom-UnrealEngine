#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

class UBlueprint;
class UEdGraph;
class UWidget;

/**
 * Auditor for Blueprint, Graph, and Widget assets.
 */
struct FATHOMUELINK_API FBlueprintGraphAuditor
{
	// --- Game-thread gather (reads UObject pointers, populates POD structs) ---

	/** Gather all audit data from a Blueprint into a POD struct. Must be called on the game thread. */
	static FBlueprintAuditData GatherBlueprintData(const UBlueprint* BP);

	/** Gather audit data from a single graph. Must be called on the game thread. */
	static FGraphAuditData GatherGraphData(const UEdGraph* Graph);

	/** Gather audit data from a widget and its children. Must be called on the game thread. */
	static FWidgetAuditData GatherWidgetData(UWidget* Widget);

	// --- Thread-safe serialization (POD to Markdown, no UObject access) ---

	/** Serialize gathered Blueprint data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FBlueprintAuditData& Data);

	/** Serialize gathered graph data to Markdown. Safe on any thread. */
	static FString SerializeGraphToMarkdown(const FGraphAuditData& Data, const FString& Prefix);

	/** Serialize gathered widget data to a Markdown indented list. Safe on any thread. */
	static FString SerializeWidgetToMarkdown(const FWidgetAuditData& Data, int32 Indent = 0);
};

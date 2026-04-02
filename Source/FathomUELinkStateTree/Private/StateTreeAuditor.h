#pragma once

#include "CoreMinimal.h"
#include "StateTreeAuditTypes.h"

class UStateTree;

/**
 * Auditor for StateTree assets.
 * Extracts state hierarchy, tasks, conditions, transitions, and evaluators
 * using direct #include access to StateTree editor headers.
 */
struct FStateTreeAuditor
{
	/** Gather all audit data from a StateTree into a POD struct. Must be called on the game thread. */
	static FStateTreeAuditData GatherData(const UStateTree* ST);

	/** Serialize gathered StateTree data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FStateTreeAuditData& Data);
};

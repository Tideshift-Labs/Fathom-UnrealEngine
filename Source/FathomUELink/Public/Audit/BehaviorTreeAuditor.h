#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

class UBehaviorTree;

/**
 * Auditor for BehaviorTree assets.
 * Extracts tree structure, blackboard keys, decorators, services, and task properties.
 */
struct FATHOMUELINK_API FBehaviorTreeAuditor
{
	/** Gather all audit data from a BehaviorTree into a POD struct. Must be called on the game thread. */
	static FBehaviorTreeAuditData GatherData(const UBehaviorTree* BT);

	/** Serialize gathered BehaviorTree data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FBehaviorTreeAuditData& Data);
};

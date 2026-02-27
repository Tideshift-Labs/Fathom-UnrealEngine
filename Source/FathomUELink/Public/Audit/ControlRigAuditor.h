#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

class UControlRigBlueprint;

/**
 * Auditor for ControlRig Blueprint assets.
 */
struct FATHOMUELINK_API FControlRigAuditor
{
	/** Gather all audit data from a ControlRig Blueprint into a POD struct. Must be called on the game thread. */
	static FControlRigAuditData GatherData(const UControlRigBlueprint* CRBP);

	/** Serialize gathered ControlRig data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FControlRigAuditData& Data);
};

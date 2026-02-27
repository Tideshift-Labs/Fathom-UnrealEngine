#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

class UUserDefinedStruct;

/**
 * Auditor for UserDefinedStruct assets.
 */
struct FATHOMUELINK_API FUserDefinedStructAuditor
{
	/** Gather all audit data from a UserDefinedStruct into a POD struct. Must be called on the game thread. */
	static FUserDefinedStructAuditData GatherData(const UUserDefinedStruct* Struct);

	/** Serialize gathered UserDefinedStruct data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FUserDefinedStructAuditData& Data);
};

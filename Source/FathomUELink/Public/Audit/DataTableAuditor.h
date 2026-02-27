#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

class UDataTable;

/**
 * Auditor for DataTable assets.
 */
struct FATHOMUELINK_API FDataTableAuditor
{
	/** Gather all audit data from a DataTable into a POD struct. Must be called on the game thread. */
	static FDataTableAuditData GatherData(const UDataTable* DataTable);

	/** Serialize gathered DataTable data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FDataTableAuditData& Data);
};

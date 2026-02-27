#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

class UDataAsset;

/**
 * Auditor for DataAsset instances.
 */
struct FATHOMUELINK_API FDataAssetAuditor
{
	/** Gather all audit data from a DataAsset into a POD struct. Must be called on the game thread. */
	static FDataAssetAuditData GatherData(const UDataAsset* Asset);

	/** Serialize gathered DataAsset data to Markdown. Computes SourceFileHash from SourceFilePath. Safe on any thread. */
	static FString SerializeToMarkdown(const FDataAssetAuditData& Data);
};

#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

class UMaterialInterface;

/**
 * Auditor for Material and MaterialInstance assets.
 */
struct FATHOMUELINK_API FMaterialAuditor
{
	/** Gather all audit data from a material into a POD struct. Must be called on the game thread. */
	static FMaterialAuditData GatherData(const UMaterialInterface* Material);

	/** Serialize gathered material data to Markdown. Safe on any thread. */
	static FString SerializeToMarkdown(const FMaterialAuditData& Data);
};

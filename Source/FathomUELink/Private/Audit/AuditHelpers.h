#pragma once

#include "CoreMinimal.h"

/**
 * Internal helpers shared across multiple domain auditors.
 * Not exported (Private header).
 */
namespace FathomAuditHelpers
{
	/**
	 * Post-process a UE exported property value to be more LLM-friendly:
	 * - NSLOCTEXT("ns", "key", "Display Text") -> "Display Text"
	 * - Trailing decimal zeros: 0.500000 -> 0.5
	 * - Default sub-structs with all-zero fields get collapsed
	 */
	FString CleanExportedValue(const FString& Raw);
}

#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

class FProperty;

/**
 * Helpers shared across multiple domain auditors. Exported so optional
 * sibling modules (e.g. FathomUELinkStateTree) can use the same formatters.
 */
namespace FathomAuditHelpers
{
	/**
	 * Post-process a UE exported property value to be more LLM-friendly:
	 * - NSLOCTEXT("ns", "key", "Display Text") -> "Display Text"
	 * - Trailing decimal zeros: 0.500000 -> 0.5
	 * - Default sub-structs with all-zero fields get collapsed
	 */
	FATHOMUELINK_API FString CleanExportedValue(const FString& Raw);

	/**
	 * Strip a UE exported object reference down to just the asset / subobject name.
	 *   /Script/Engine.Texture2D'/Game/Foo/Bar/T_X.T_X' -> "T_X"
	 *   /Game/.../Foo.Foo:Sub_1                         -> "Sub_1"
	 *   None / empty                                    -> unchanged
	 */
	FATHOMUELINK_API FString StripObjectPathToAssetName(const FString& Raw);

	/**
	 * True when the property references reflection metadata that no longer
	 * exists (e.g. a TSubclassOf/TSoftClassPtr/enum/struct whose backing asset
	 * was deleted, leaving MetaClass/Enum/Struct null). The engine's
	 * GetCPPType, ExportTextItem, and Identical implementations check() or
	 * dereference these unguarded, so such properties must be skipped.
	 * Recurses into container inner properties and struct fields.
	 */
	FATHOMUELINK_API bool HasBrokenTypeMetadata(const FProperty* Prop);

	/**
	 * GetCPPType that returns "Unknown" instead of crashing when the
	 * property's type metadata is broken (see HasBrokenTypeMetadata).
	 */
	FATHOMUELINK_API FString GetSafeCPPType(const FProperty* Prop, FString* ExtendedTypeText = nullptr);

	/**
	 * Format a property value for Markdown output, recursing into structs,
	 * arrays, sets, maps, and dynamic-schema wrappers (FInstancedStruct and
	 * FInstancedPropertyBag). The dynamic wrappers are unwrapped to their
	 * runtime UScriptStruct + memory so their actual contents surface in
	 * the audit; see docs/learnings/statetree-instanced-property-decoding.md.
	 *
	 * Returns either:
	 *   - a single-line scalar string (no trailing newline), or
	 *   - a multi-line block where every line is indented at IndentDepth*2
	 *     spaces and the block ends with a newline.
	 *
	 * Callers detect multi-line by checking for '\n' and decide how to splice
	 * the block under their parent bullet.
	 */
	FATHOMUELINK_API FString FormatPropertyValue(const FProperty* Prop, const void* ValuePtr, int32 IndentDepth = 0);

	/** Rendering style for SerializePropertyOverridesToMarkdown. */
	struct FPropertyRenderStyle
	{
		/** Leading indent prepended to every line (e.g. "" for top-level, "  " for nested). */
		FString Indent;
		/** If true, each property line begins with "- " (Markdown bullet). */
		bool bUseBullet = true;
		/** Separator for single-line values (e.g. " = " or ": "). Multi-line always uses ":". */
		FString InlineSeparator = TEXT(" = ");
	};

	/**
	 * Render a list of FPropertyOverrideData entries to Markdown, handling both
	 * single-line and multi-line (newline-containing) values uniformly.
	 *
	 * For multi-line values, every line of Value is re-prefixed with Style.Indent + "  "
	 * so the block sits indented under its parent bullet. The Value's own internal
	 * indentation (from FormatPropertyValue) is preserved on top of that.
	 */
	FATHOMUELINK_API void SerializePropertyOverridesToMarkdown(
		FString& Out,
		const TArray<FPropertyOverrideData>& Props,
		const FPropertyRenderStyle& Style);
}

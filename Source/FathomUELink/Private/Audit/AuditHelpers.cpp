#include "Audit/AuditHelpers.h"

#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"

FString FathomAuditHelpers::CleanExportedValue(const FString& Raw)
{
	FString Result = Raw;

	// --- Simplify NSLOCTEXT to just the display string ---
	// Pattern: NSLOCTEXT("...", "...", "actual text")
	int32 SearchFrom = 0;
	while (true)
	{
		const int32 NsPos = Result.Find(TEXT("NSLOCTEXT("), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
		if (NsPos == INDEX_NONE) break;

		// Find the third quoted string (the display text)
		int32 QuoteCount = 0;
		int32 DisplayStart = INDEX_NONE;
		int32 DisplayEnd = INDEX_NONE;
		bool bInQuote = false;
		bool bEscaped = false;

		for (int32 i = NsPos + 10; i < Result.Len(); ++i)  // skip "NSLOCTEXT("
		{
			const TCHAR Ch = Result[i];
			if (bEscaped)
			{
				bEscaped = false;
				continue;
			}
			if (Ch == TEXT('\\'))
			{
				bEscaped = true;
				continue;
			}
			if (Ch == TEXT('"'))
			{
				if (!bInQuote)
				{
					bInQuote = true;
					++QuoteCount;
					if (QuoteCount == 3)
					{
						DisplayStart = i + 1;
					}
				}
				else
				{
					bInQuote = false;
					if (QuoteCount == 3)
					{
						DisplayEnd = i;
					}
				}
			}
			if (Ch == TEXT(')') && !bInQuote && QuoteCount >= 3)
			{
				// Replace the whole NSLOCTEXT(...) with just the quoted display text
				if (DisplayStart != INDEX_NONE && DisplayEnd != INDEX_NONE)
				{
					const FString DisplayText = TEXT("\"") + Result.Mid(DisplayStart, DisplayEnd - DisplayStart) + TEXT("\"");
					Result = Result.Left(NsPos) + DisplayText + Result.Mid(i + 1);
					SearchFrom = NsPos + DisplayText.Len();
				}
				else
				{
					SearchFrom = i + 1;
				}
				break;
			}
		}

		// Safety: if we didn't break out of the loop, stop to avoid infinite loop
		if (SearchFrom <= NsPos)
		{
			break;
		}
	}

	// --- Trim trailing decimal zeros: 0.500000 -> 0.5, 1.000000 -> 1.0 ---
	// Match patterns like digits.digits000 and trim, keeping at least one decimal
	{
		FString Cleaned;
		Cleaned.Reserve(Result.Len());

		int32 i = 0;
		while (i < Result.Len())
		{
			// Look for a decimal number pattern
			if (FChar::IsDigit(Result[i]))
			{
				int32 NumStart = i;
				// Consume integer part
				while (i < Result.Len() && FChar::IsDigit(Result[i])) ++i;

				if (i < Result.Len() && Result[i] == TEXT('.'))
				{
					++i; // consume the dot
					int32 DecStart = i;
					while (i < Result.Len() && FChar::IsDigit(Result[i])) ++i;
					int32 DecEnd = i;

					// Check next char is NOT alphanumeric (to avoid modifying identifiers)
					if (DecEnd > DecStart && (i >= Result.Len() || !FChar::IsAlnum(Result[i])))
					{
						// Trim trailing zeros, but keep at least one digit after dot
						int32 TrimEnd = DecEnd;
						while (TrimEnd > DecStart + 1 && Result[TrimEnd - 1] == TEXT('0'))
						{
							--TrimEnd;
						}
						Cleaned += Result.Mid(NumStart, DecStart - NumStart); // integer part + dot
						Cleaned += Result.Mid(DecStart, TrimEnd - DecStart);  // trimmed decimals
					}
					else
					{
						// Not a float we want to trim, keep as-is
						Cleaned += Result.Mid(NumStart, i - NumStart);
					}
				}
				else
				{
					Cleaned += Result.Mid(NumStart, i - NumStart);
				}
			}
			else
			{
				Cleaned += Result[i];
				++i;
			}
		}
		Result = MoveTemp(Cleaned);
	}

	// --- Strip known all-default sub-structs ---
	// Remove patterns like: Margin=(), OutlineSettings=(...all zeros...), UVRegion=(...all zeros...)
	// Strategy: remove key=(...) blocks where the parenthesized content is all zeros/defaults
	{
		// Common default patterns to strip entirely when they appear as key=value
		static const TArray<FString> DefaultPatterns = {
			TEXT(",Margin=()"),
			TEXT(",bIsValid=False"),
			TEXT(",ImageSize=(X=32.0,Y=32.0)"),
		};

		for (const FString& Pat : DefaultPatterns)
		{
			Result.ReplaceInline(*Pat, TEXT(""));
		}

		// Strip sub-struct blocks whose numeric values are all known defaults.
		// Parses identifiers (property names, enum values) as opaque tokens and
		// checks every numeric literal is 0, 1, or 32 (common FSlateBrush
		// defaults: zero vectors, white tint, 32x32 image size).
		auto IsAllDefaultContent = [](const FString& Content) -> bool
		{
			int32 j = 0;
			while (j < Content.Len())
			{
				const TCHAR C = Content[j];
				if (FChar::IsDigit(C))
				{
					const int32 NumStart = j;
					while (j < Content.Len() && FChar::IsDigit(Content[j])) ++j;
					if (j < Content.Len() && Content[j] == TEXT('.'))
					{
						++j;
						while (j < Content.Len() && FChar::IsDigit(Content[j])) ++j;
					}
					const double Val = FCString::Atod(*Content.Mid(NumStart, j - NumStart));
					if (Val != 0.0 && Val != 1.0)
					{
						return false;
					}
				}
				else if (FChar::IsAlpha(C) || C == TEXT('_'))
				{
					while (j < Content.Len() && (FChar::IsAlnum(Content[j]) || Content[j] == TEXT('_'))) ++j;
				}
				else if (C == TEXT(',') || C == TEXT('=') || C == TEXT('(') || C == TEXT(')') || C == TEXT(' '))
				{
					++j;
				}
				else
				{
					return false;
				}
			}
			return true;
		};

		// Look for ,SubStruct=(...) blocks with default-only content
		static const TArray<FString> SubStructPrefixes = {
			TEXT(",OverrideBrush="),
			TEXT(",OutlineSettings="),
			TEXT(",UVRegion="),
		};

		for (const FString& Prefix : SubStructPrefixes)
		{
			int32 Pos = 0;
			while (true)
			{
				Pos = Result.Find(Prefix, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
				if (Pos == INDEX_NONE) break;

				// Find the matching close paren
				int32 ParenStart = Pos + Prefix.Len();
				if (ParenStart < Result.Len() && Result[ParenStart] == TEXT('('))
				{
					int32 Depth = 1;
					int32 ParenEnd = ParenStart + 1;
					while (ParenEnd < Result.Len() && Depth > 0)
					{
						if (Result[ParenEnd] == TEXT('(')) ++Depth;
						else if (Result[ParenEnd] == TEXT(')')) --Depth;
						++ParenEnd;
					}

					const FString Content = Result.Mid(ParenStart + 1, ParenEnd - ParenStart - 2);
					if (IsAllDefaultContent(Content))
					{
						Result = Result.Left(Pos) + Result.Mid(ParenEnd);
						continue; // don't advance Pos, check same position for more
					}
				}

				Pos += Prefix.Len();
			}
		}
	}

	return Result;
}

// ---------------------------------------------------------------------------
// HasBrokenTypeMetadata / GetSafeCPPType
// ---------------------------------------------------------------------------

bool FathomAuditHelpers::HasBrokenTypeMetadata(const FProperty* Prop)
{
	if (!Prop)
	{
		return true;
	}
	// Most-derived classes first: FClassProperty derives from FObjectProperty,
	// FSoftClassProperty from FSoftObjectProperty/FObjectPropertyBase.
	if (const FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		return SoftClassProp->MetaClass == nullptr || SoftClassProp->PropertyClass == nullptr;
	}
	if (const FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		return ClassProp->MetaClass == nullptr || ClassProp->PropertyClass == nullptr;
	}
	if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		return ObjProp->PropertyClass == nullptr;
	}
	if (const FInterfaceProperty* IfaceProp = CastField<FInterfaceProperty>(Prop))
	{
		return IfaceProp->InterfaceClass == nullptr;
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		return EnumProp->GetEnum() == nullptr;
	}
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (StructProp->Struct == nullptr)
		{
			return true;
		}
		// A struct type cannot contain itself at the C++ level, so this
		// recursion terminates.
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			if (HasBrokenTypeMetadata(*It))
			{
				return true;
			}
		}
		return false;
	}
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		return HasBrokenTypeMetadata(ArrayProp->Inner);
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		return HasBrokenTypeMetadata(SetProp->ElementProp);
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		return HasBrokenTypeMetadata(MapProp->KeyProp) || HasBrokenTypeMetadata(MapProp->ValueProp);
	}
	return false;
}

FString FathomAuditHelpers::GetSafeCPPType(const FProperty* Prop, FString* ExtendedTypeText)
{
	if (HasBrokenTypeMetadata(Prop))
	{
		return TEXT("Unknown");
	}
	return Prop->GetCPPType(ExtendedTypeText);
}

// ---------------------------------------------------------------------------
// StripObjectPathToAssetName / FormatPropertyValue
// ---------------------------------------------------------------------------

namespace FathomAuditHelpers
{
	static FString MakeIndent(int32 Depth)
	{
		return FString::ChrN(FMath::Max(Depth, 0) * 2, TEXT(' '));
	}

	/**
	 * Recursion state for instanced-subobject expansion. Visited tracks the active
	 * stack to detect cycles; Depth caps runaway nesting. Both are needed because
	 * Instanced UPROPERTY chains (e.g. UGameplayEffect::GEComponents -> per-config
	 * subobjects) can in principle reference back into themselves.
	 */
	static constexpr int32 MaxInstancedRecursionDepth = 8;

	struct FFormatContext
	{
		TSet<const UObject*> Visited;
		int32 InstancedDepth = 0;
	};

	// Forward decls for mutual recursion.
	static FString FormatPropertyValueImpl(const FProperty* Prop, const void* ValuePtr, int32 IndentDepth, FFormatContext& Ctx);
	static FString FormatStructValue(const UScriptStruct* StructType, const void* StructPtr, int32 IndentDepth, FFormatContext& Ctx);
	static FString FormatInstancedObjectFields(const UObject* Object, int32 IndentDepth, FFormatContext& Ctx);
	static FString GetInstancedObjectLabel(const UObject* Object);

	/**
	 * Decode an FInstancedPropertyBag's dynamic schema and emit its current values.
	 * Bags carry a runtime-built UScriptStruct, so static FProperty walking on the
	 * bag itself yields nothing useful. Once unwrapped via GetValue(), the inner
	 * struct view feeds straight into FormatStructValue.
	 */
	static FString FormatPropertyBagValue(const FInstancedPropertyBag& Bag, int32 IndentDepth, FFormatContext& Ctx);

	/**
	 * True when Prop owns its referenced UObject and we should recurse into the
	 * subobject's fields instead of emitting a path string. Detection relies on
	 * CPF_InstancedReference, which UE sets for UPROPERTY(Instanced ...) and
	 * propagates onto array/set/map inner properties. External asset refs
	 * (e.g. UTexture2D*) never carry this flag, so they keep the existing
	 * path-string behavior.
	 */
	static bool IsInstancedObjectProperty(const FProperty* Prop)
	{
		if (!Prop) return false;
		const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop);
		if (!ObjProp) return false;
		return Prop->HasAnyPropertyFlags(CPF_InstancedReference);
	}

	/**
	 * If Prop is an enum-typed property and Exported is a recognized internal enum
	 * name, resolve to the enum's display-name metadata. Returns Exported unchanged
	 * otherwise. Handles user-defined enums where internal names are autogenerated
	 * (e.g. "NewEnumerator0") but display names carry the human-meaningful text.
	 */
	static FString TryResolveEnumDisplayName(const FProperty* Prop, const FString& Exported)
	{
		const UEnum* Enum = nullptr;
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			Enum = EnumProp->GetEnum();
		}
		else if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			Enum = ByteProp->Enum;
		}
		if (!Enum || Exported.IsEmpty())
		{
			return Exported;
		}
		const int64 EnumValue = Enum->GetValueByNameString(Exported);
		if (EnumValue == INDEX_NONE)
		{
			return Exported;
		}
		const FText DisplayName = Enum->GetDisplayNameTextByValue(EnumValue);
		if (DisplayName.IsEmpty())
		{
			return Exported;
		}
		return DisplayName.ToString();
	}

	/** Format a scalar (non-array, non-struct, non-set, non-map) property at value level. */
	static FString FormatScalarProperty(const FProperty* Prop, const void* ValuePtr)
	{
		// Broken type metadata (deleted class/enum assets) makes ExportTextItem
		// check()-fail or dereference null inside the engine.
		if (!Prop || !ValuePtr || HasBrokenTypeMetadata(Prop))
		{
			return TEXT("(unavailable)");
		}

		FString Exported;
		Prop->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);

		// Strip /Script/Module.Class'/Path/Asset.Asset' wrappers for object refs.
		if (Prop->IsA<FObjectPropertyBase>() || Prop->IsA<FSoftObjectProperty>())
		{
			if (Exported.IsEmpty() || Exported == TEXT("None"))
			{
				return Exported;
			}
			return StripObjectPathToAssetName(Exported);
		}

		// Resolve enum internal name (e.g. "NewEnumerator0") to display name (e.g.
		// "Curvy figure"). Applies recursively when enums are nested in arrays/structs.
		const FString Resolved = TryResolveEnumDisplayName(Prop, Exported);

		return CleanExportedValue(Resolved);
	}
}

FString FathomAuditHelpers::StripObjectPathToAssetName(const FString& Raw)
{
	FString Trimmed = Raw.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || Trimmed == TEXT("None"))
	{
		return Trimmed;
	}

	// If wrapped as Class'inner', slice out the inner content.
	const int32 FirstQuote = Trimmed.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	const int32 LastQuote = Trimmed.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (FirstQuote != INDEX_NONE && LastQuote != INDEX_NONE && LastQuote > FirstQuote)
	{
		Trimmed = Trimmed.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
	}

	// Take the substring after the rightmost of '.', ':', '/'.
	const int32 LastDot = Trimmed.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const int32 LastColon = Trimmed.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const int32 LastSlash = Trimmed.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const int32 Cut = FMath::Max3(LastDot, LastColon, LastSlash);
	if (Cut != INDEX_NONE && Cut + 1 < Trimmed.Len())
	{
		Trimmed = Trimmed.Mid(Cut + 1);
	}

	return Trimmed.IsEmpty() ? Raw : Trimmed;
}

FString FathomAuditHelpers::FormatPropertyValue(const FProperty* Prop, const void* ValuePtr, int32 IndentDepth)
{
	FFormatContext Ctx;
	return FormatPropertyValueImpl(Prop, ValuePtr, IndentDepth, Ctx);
}

namespace FathomAuditHelpers
{
	/**
	 * Render an instanced-object array/set element: "N. <Label>" header line
	 * followed by the subobject's expanded field block. Shared between the
	 * array and set branches.
	 */
	static FString FormatInstancedContainerElement(
		const UObject* Object,
		int32 OrdinalIndex,
		int32 IndentDepth,
		FFormatContext& Ctx,
		const FString& ItemIndent)
	{
		const FString Label = GetInstancedObjectLabel(Object);
		const FString Fields = FormatInstancedObjectFields(Object, IndentDepth + 1, Ctx);
		if (Fields.IsEmpty())
		{
			return FString::Printf(TEXT("%s%d. %s (default)\n"), *ItemIndent, OrdinalIndex, *Label);
		}
		return FString::Printf(TEXT("%s%d. %s\n%s"), *ItemIndent, OrdinalIndex, *Label, *Fields);
	}

	static FString FormatPropertyValueImpl(const FProperty* Prop, const void* ValuePtr, int32 IndentDepth, FFormatContext& Ctx)
	{
		if (!Prop || !ValuePtr)
		{
			return FString();
		}

		// --- Array ---
		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			FScriptArrayHelper Helper(ArrayProp, ValuePtr);
			const int32 N = Helper.Num();
			if (N == 0)
			{
				return TEXT("[]");
			}

			const FProperty* Inner = ArrayProp->Inner;
			const FString ItemIndent = MakeIndent(IndentDepth);
			FString Result;
			for (int32 i = 0; i < N; ++i)
			{
				const void* ElemPtr = Helper.GetRawPtr(i);
				if (const FStructProperty* StructInner = CastField<FStructProperty>(Inner))
				{
					const FString StructLines = FormatStructValue(StructInner->Struct, ElemPtr, IndentDepth + 1, Ctx);
					if (StructLines.IsEmpty())
					{
						// Element matches default for every field; render as "N. (default)" so the
						// element count is still preserved.
						Result += FString::Printf(TEXT("%s%d. (default)\n"), *ItemIndent, i + 1);
					}
					else
					{
						Result += FString::Printf(TEXT("%s%d.\n%s"), *ItemIndent, i + 1, *StructLines);
					}
				}
				else if (IsInstancedObjectProperty(Inner))
				{
					const FObjectProperty* ObjInner = CastFieldChecked<FObjectProperty>(Inner);
					const UObject* SubObj = ObjInner->GetObjectPropertyValue(ElemPtr);
					if (!SubObj)
					{
						Result += FString::Printf(TEXT("%s%d. None\n"), *ItemIndent, i + 1);
					}
					else
					{
						Result += FormatInstancedContainerElement(SubObj, i + 1, IndentDepth, Ctx, ItemIndent);
					}
				}
				else
				{
					const FString Inline = FormatScalarProperty(Inner, ElemPtr);
					Result += FString::Printf(TEXT("%s%d. %s\n"), *ItemIndent, i + 1, *Inline);
				}
			}
			return Result;
		}

		// --- Set ---
		if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			FScriptSetHelper Helper(SetProp, ValuePtr);
			if (Helper.Num() == 0)
			{
				return TEXT("[]");
			}

			const FProperty* Inner = SetProp->ElementProp;
			const FString ItemIndent = MakeIndent(IndentDepth);
			FString Result;
			int32 OrdinalIndex = 0;
			for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
			{
				if (!Helper.IsValidIndex(i))
				{
					continue;
				}
				const void* ElemPtr = Helper.GetElementPtr(i);
				++OrdinalIndex;
				if (const FStructProperty* StructInner = CastField<FStructProperty>(Inner))
				{
					const FString StructLines = FormatStructValue(StructInner->Struct, ElemPtr, IndentDepth + 1, Ctx);
					if (StructLines.IsEmpty())
					{
						Result += FString::Printf(TEXT("%s%d. (default)\n"), *ItemIndent, OrdinalIndex);
					}
					else
					{
						Result += FString::Printf(TEXT("%s%d.\n%s"), *ItemIndent, OrdinalIndex, *StructLines);
					}
				}
				else if (IsInstancedObjectProperty(Inner))
				{
					const FObjectProperty* ObjInner = CastFieldChecked<FObjectProperty>(Inner);
					const UObject* SubObj = ObjInner->GetObjectPropertyValue(ElemPtr);
					if (!SubObj)
					{
						Result += FString::Printf(TEXT("%s%d. None\n"), *ItemIndent, OrdinalIndex);
					}
					else
					{
						Result += FormatInstancedContainerElement(SubObj, OrdinalIndex, IndentDepth, Ctx, ItemIndent);
					}
				}
				else
				{
					const FString Inline = FormatScalarProperty(Inner, ElemPtr);
					Result += FString::Printf(TEXT("%s%d. %s\n"), *ItemIndent, OrdinalIndex, *Inline);
				}
			}
			return Result;
		}

		// --- Map ---
		if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			FScriptMapHelper Helper(MapProp, ValuePtr);
			if (Helper.Num() == 0)
			{
				return TEXT("{}");
			}

			const FProperty* KeyProp = MapProp->KeyProp;
			const FProperty* ValProp = MapProp->ValueProp;
			const FString ItemIndent = MakeIndent(IndentDepth);
			FString Result;
			for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
			{
				if (!Helper.IsValidIndex(i))
				{
					continue;
				}
				const void* KeyPtr = Helper.GetKeyPtr(i);
				const void* ValPtr = Helper.GetValuePtr(i);

				// Keys are typically scalar; render inline.
				const FString KeyStr = FormatScalarProperty(KeyProp, KeyPtr);
				const FString ValStr = FormatPropertyValueImpl(ValProp, ValPtr, IndentDepth + 1, Ctx);

				if (ValStr.Contains(TEXT("\n")))
				{
					Result += FString::Printf(TEXT("%s- %s:\n%s"), *ItemIndent, *KeyStr, *ValStr);
				}
				else
				{
					Result += FString::Printf(TEXT("%s- %s: %s\n"), *ItemIndent, *KeyStr, *ValStr);
				}
			}
			return Result;
		}

		// --- Struct ---
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			return FormatStructValue(StructProp->Struct, ValuePtr, IndentDepth, Ctx);
		}

		// --- Instanced UObject (UPROPERTY Instanced) ---
		if (IsInstancedObjectProperty(Prop))
		{
			const FObjectProperty* ObjProp = CastFieldChecked<FObjectProperty>(Prop);
			const UObject* SubObj = ObjProp->GetObjectPropertyValue(ValuePtr);
			if (!SubObj)
			{
				return TEXT("None");
			}
			const FString Label = GetInstancedObjectLabel(SubObj);
			const FString Fields = FormatInstancedObjectFields(SubObj, IndentDepth + 1, Ctx);
			if (Fields.IsEmpty())
			{
				return Label;
			}
			const FString HeaderIndent = MakeIndent(IndentDepth);
			return FString::Printf(TEXT("%s%s\n%s"), *HeaderIndent, *Label, *Fields);
		}

		// --- Scalar fallback ---
		return FormatScalarProperty(Prop, ValuePtr);
	}
}

namespace FathomAuditHelpers
{
	static FString FormatStructValue(const UScriptStruct* StructType, const void* StructPtr, int32 IndentDepth, FFormatContext& Ctx)
	{
		if (!StructType || !StructPtr)
		{
			return FString();
		}

		// Unwrap dynamic-schema wrappers so all callers (including TArray /
		// TSet / TMap element walkers, not just FStructProperty in
		// FormatPropertyValueImpl) get bag and instanced-struct contents
		// surfaced uniformly. No infinite recursion risk: bag inner types
		// are distinct USTRUCTs, and FInstancedStruct cannot contain itself
		// at the C++ type level.
		if (StructType == FInstancedPropertyBag::StaticStruct())
		{
			const FInstancedPropertyBag* Bag = static_cast<const FInstancedPropertyBag*>(StructPtr);
			return FormatPropertyBagValue(*Bag, IndentDepth, Ctx);
		}
		if (StructType == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct* Inst = static_cast<const FInstancedStruct*>(StructPtr);
			if (const UScriptStruct* InnerType = Inst->GetScriptStruct())
			{
				return FormatStructValue(InnerType, Inst->GetMemory(), IndentDepth, Ctx);
			}
			return TEXT("None");
		}

		// Allocate a default-initialized struct so we can skip per-field defaults.
		// Zero-size structs (empty/dynamic schemas) would crash InitializeStruct's
		// check(Dest), so bail early; an empty struct has nothing to emit anyway.
		const int32 StructSize = StructType->GetStructureSize();
		if (StructSize <= 0)
		{
			return FString();
		}
		TArray<uint8> DefaultBuffer;
		DefaultBuffer.SetNumZeroed(StructSize);
		StructType->InitializeStruct(DefaultBuffer.GetData());

		const FString FieldIndent = MakeIndent(IndentDepth);
		FString Result;

		for (TFieldIterator<FProperty> FieldIt(StructType); FieldIt; ++FieldIt)
		{
			const FProperty* Field = *FieldIt;
			if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			// Identical() dereferences type metadata (e.g. FStructProperty::Struct)
			// unguarded; skip fields whose backing types were deleted.
			if (HasBrokenTypeMetadata(Field))
			{
				continue;
			}

			const void* FieldPtr = Field->ContainerPtrToValuePtr<void>(StructPtr);
			const void* DefaultFieldPtr = Field->ContainerPtrToValuePtr<void>(DefaultBuffer.GetData());

			if (Field->Identical(FieldPtr, DefaultFieldPtr))
			{
				continue;
			}

			FString Sub = FormatPropertyValueImpl(Field, FieldPtr, IndentDepth + 1, Ctx);
			if (Sub.IsEmpty() || Sub == TEXT("()") || Sub == TEXT("None"))
			{
				continue;
			}

			if (Sub.Contains(TEXT("\n")))
			{
				Result += FString::Printf(TEXT("%s- %s:\n%s"), *FieldIndent, *Field->GetAuthoredName(), *Sub);
			}
			else
			{
				Result += FString::Printf(TEXT("%s- %s: %s\n"), *FieldIndent, *Field->GetAuthoredName(), *Sub);
			}
		}

		StructType->DestroyStruct(DefaultBuffer.GetData());
		return Result;
	}

	/**
	 * Walk a property bag's dynamic schema directly, without the default-comparison
	 * buffer trick used in FormatStructValue. UPropertyBag overrides InitializeStruct
	 * to increment a refcount and asserts when handed a zero-size buffer (empty bag),
	 * so we can't use the same path. Bags also represent values the owner explicitly
	 * set, so emitting all populated fields is the desired behavior anyway.
	 */
	static FString FormatPropertyBagValue(const FInstancedPropertyBag& Bag, int32 IndentDepth, FFormatContext& Ctx)
	{
		const FConstStructView View = Bag.GetValue();
		const UScriptStruct* StructType = View.GetScriptStruct();
		const uint8* Memory = View.GetMemory();
		if (!StructType || !Memory)
		{
			return FString();
		}

		const FString FieldIndent = MakeIndent(IndentDepth);
		FString Result;

		for (TFieldIterator<FProperty> FieldIt(StructType); FieldIt; ++FieldIt)
		{
			const FProperty* Field = *FieldIt;
			if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			if (HasBrokenTypeMetadata(Field))
			{
				continue;
			}

			const void* FieldPtr = Field->ContainerPtrToValuePtr<void>(Memory);

			FString Sub = FormatPropertyValueImpl(Field, FieldPtr, IndentDepth + 1, Ctx);
			if (Sub.IsEmpty() || Sub == TEXT("()") || Sub == TEXT("None"))
			{
				continue;
			}

			if (Sub.Contains(TEXT("\n")))
			{
				Result += FString::Printf(TEXT("%s- %s:\n%s"), *FieldIndent, *Field->GetAuthoredName(), *Sub);
			}
			else
			{
				Result += FString::Printf(TEXT("%s- %s: %s\n"), *FieldIndent, *Field->GetAuthoredName(), *Sub);
			}
		}

		return Result;
	}

	/**
	 * Read a non-empty EditorFriendlyName off the subobject if the class declares
	 * one (GAS UGameplayEffectComponent does, transient). Falls back to class name.
	 * The friendly name is what shows up in the editor index, e.g. "Grant Abilities
	 * While Active" rather than "GameplayEffectComponent_GrantAbilitiesWhileActive".
	 */
	static FString GetInstancedObjectLabel(const UObject* Object)
	{
		if (!Object)
		{
			return TEXT("None");
		}
		const UClass* Class = Object->GetClass();
		const FString ClassName = Class ? Class->GetName() : TEXT("UObject");

		if (Class)
		{
			if (const FProperty* FriendlyProp = Class->FindPropertyByName(TEXT("EditorFriendlyName")))
			{
				if (const FStrProperty* StrProp = CastField<FStrProperty>(FriendlyProp))
				{
					const FString FriendlyValue = StrProp->GetPropertyValue_InContainer(Object);
					if (!FriendlyValue.IsEmpty())
					{
						return FString::Printf(TEXT("%s (%s)"), *FriendlyValue, *ClassName);
					}
				}
			}
		}
		return ClassName;
	}

	/**
	 * Walk the subobject's UClass properties and emit field bullets, skipping
	 * fields that match the class CDO. Cycle and depth guards via Ctx; a hit
	 * either guard returns a sentinel string instead of recursing.
	 */
	static FString FormatInstancedObjectFields(const UObject* Object, int32 IndentDepth, FFormatContext& Ctx)
	{
		if (!Object)
		{
			return FString();
		}

		const UClass* Class = Object->GetClass();
		if (!Class || Class->IsChildOf<UClass>() || Class->IsChildOf<UPackage>())
		{
			return FString();
		}

		const FString FieldIndent = MakeIndent(IndentDepth);

		if (Ctx.InstancedDepth >= MaxInstancedRecursionDepth)
		{
			return FString::Printf(TEXT("%s- (depth limit reached)\n"), *FieldIndent);
		}
		if (Ctx.Visited.Contains(Object))
		{
			return FString::Printf(TEXT("%s- (cycle: %s)\n"), *FieldIndent, *Class->GetName());
		}

		Ctx.Visited.Add(Object);
		++Ctx.InstancedDepth;
		ON_SCOPE_EXIT
		{
			Ctx.Visited.Remove(Object);
			--Ctx.InstancedDepth;
		};

		const UObject* CDO = Class->GetDefaultObject();
		FString Result;

		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			const FProperty* Field = *It;
			if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			if (!Field->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}
			if (HasBrokenTypeMetadata(Field))
			{
				continue;
			}

			const void* FieldPtr = Field->ContainerPtrToValuePtr<void>(Object);
			const void* DefaultPtr = CDO ? Field->ContainerPtrToValuePtr<void>(CDO) : nullptr;
			if (DefaultPtr && Field->Identical(FieldPtr, DefaultPtr))
			{
				continue;
			}

			FString Sub = FormatPropertyValueImpl(Field, FieldPtr, IndentDepth + 1, Ctx);
			if (Sub.IsEmpty() || Sub == TEXT("()") || Sub == TEXT("None"))
			{
				continue;
			}

			if (Sub.Contains(TEXT("\n")))
			{
				Result += FString::Printf(TEXT("%s- %s:\n%s"), *FieldIndent, *Field->GetAuthoredName(), *Sub);
			}
			else
			{
				Result += FString::Printf(TEXT("%s- %s: %s\n"), *FieldIndent, *Field->GetAuthoredName(), *Sub);
			}
		}

		return Result;
	}
}

void FathomAuditHelpers::SerializePropertyOverridesToMarkdown(
	FString& Out,
	const TArray<FPropertyOverrideData>& Props,
	const FPropertyRenderStyle& Style)
{
	const FString BulletPrefix = Style.bUseBullet ? TEXT("- ") : TEXT("");
	const FString MultiLineChildIndent = Style.Indent + TEXT("  ");

	for (const FPropertyOverrideData& Prop : Props)
	{
		if (Prop.Value.Contains(TEXT("\n")))
		{
			Out += FString::Printf(TEXT("%s%s%s:\n"), *Style.Indent, *BulletPrefix, *Prop.Name);

			// Re-prefix every non-empty line of Value with MultiLineChildIndent so the
			// block sits indented under the parent bullet.
			int32 LineStart = 0;
			while (LineStart < Prop.Value.Len())
			{
				int32 LineEnd = Prop.Value.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, LineStart);
				if (LineEnd == INDEX_NONE)
				{
					LineEnd = Prop.Value.Len();
				}
				const FString Line = Prop.Value.Mid(LineStart, LineEnd - LineStart);
				if (!Line.IsEmpty())
				{
					Out += MultiLineChildIndent + Line + TEXT("\n");
				}
				LineStart = LineEnd + 1;
			}
		}
		else
		{
			Out += FString::Printf(TEXT("%s%s%s%s%s\n"),
				*Style.Indent, *BulletPrefix, *Prop.Name, *Style.InlineSeparator, *Prop.Value);
		}
	}
}

#include "Audit/AuditHelpers.h"

#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/Class.h"

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
// StripObjectPathToAssetName / FormatPropertyValue
// ---------------------------------------------------------------------------

namespace FathomAuditHelpers
{
	static FString MakeIndent(int32 Depth)
	{
		return FString::ChrN(FMath::Max(Depth, 0) * 2, TEXT(' '));
	}

	// Forward decl for mutual recursion with FormatPropertyValue.
	static FString FormatStructValue(const UScriptStruct* StructType, const void* StructPtr, int32 IndentDepth);

	/** Format a scalar (non-array, non-struct, non-set, non-map) property at value level. */
	static FString FormatScalarProperty(const FProperty* Prop, const void* ValuePtr)
	{
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

		return CleanExportedValue(Exported);
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
				const FString StructLines = FormatStructValue(StructInner->Struct, ElemPtr, IndentDepth + 1);
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
				const FString StructLines = FormatStructValue(StructInner->Struct, ElemPtr, IndentDepth + 1);
				if (StructLines.IsEmpty())
				{
					Result += FString::Printf(TEXT("%s%d. (default)\n"), *ItemIndent, OrdinalIndex);
				}
				else
				{
					Result += FString::Printf(TEXT("%s%d.\n%s"), *ItemIndent, OrdinalIndex, *StructLines);
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
			const FString ValStr = FormatPropertyValue(ValProp, ValPtr, IndentDepth + 1);

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
		return FormatStructValue(StructProp->Struct, ValuePtr, IndentDepth);
	}

	// --- Scalar fallback ---
	return FormatScalarProperty(Prop, ValuePtr);
}

namespace FathomAuditHelpers
{
	static FString FormatStructValue(const UScriptStruct* StructType, const void* StructPtr, int32 IndentDepth)
	{
		if (!StructType || !StructPtr)
		{
			return FString();
		}

		// Allocate a default-initialized struct so we can skip per-field defaults.
		const int32 StructSize = StructType->GetStructureSize();
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

			const void* FieldPtr = Field->ContainerPtrToValuePtr<void>(StructPtr);
			const void* DefaultFieldPtr = Field->ContainerPtrToValuePtr<void>(DefaultBuffer.GetData());

			if (Field->Identical(FieldPtr, DefaultFieldPtr))
			{
				continue;
			}

			FString Sub = FormatPropertyValue(Field, FieldPtr, IndentDepth + 1);
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

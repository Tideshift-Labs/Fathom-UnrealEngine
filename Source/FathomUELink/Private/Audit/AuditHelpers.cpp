#include "Audit/AuditHelpers.h"

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

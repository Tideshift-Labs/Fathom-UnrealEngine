#include "Audit/DataTableAuditor.h"

#include "Audit/AuditHelpers.h"
#include "Audit/AuditFileUtils.h"
#include "Engine/DataTable.h"
#include "UObject/UnrealType.h"

FDataTableAuditData FDataTableAuditor::GatherData(const UDataTable* DataTable)
{
	FDataTableAuditData Data;

	Data.Name = DataTable->GetName();
	Data.Path = DataTable->GetPathName();
	Data.PackageName = DataTable->GetOutermost()->GetName();
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	// Row struct info
	if (const UScriptStruct* RowStruct = DataTable->GetRowStruct())
	{
		Data.RowStructName = RowStruct->GetName();
		Data.RowStructPath = RowStruct->GetPathName();

		// Column schema from struct properties
		for (TFieldIterator<FProperty> PropIt(RowStruct); PropIt; ++PropIt)
		{
			FDataTableColumnDef Col;
			Col.Name = PropIt->GetName();
			FString ExtendedType;
			Col.Type = PropIt->GetCPPType(&ExtendedType);
			Col.Type += ExtendedType;
			Data.Columns.Add(MoveTemp(Col));
		}
	}

	// Row data
	const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();
	const UScriptStruct* RowStruct = DataTable->GetRowStruct();

	for (const auto& Pair : RowMap)
	{
		FDataTableRowData RowData;
		RowData.RowName = Pair.Key.ToString();

		if (RowStruct && Pair.Value)
		{
			for (TFieldIterator<FProperty> PropIt(RowStruct); PropIt; ++PropIt)
			{
				const FProperty* Prop = *PropIt;
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Pair.Value);
				FString ValueStr;
				Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
				RowData.Values.Add(MoveTemp(ValueStr));
			}
		}

		Data.Rows.Add(MoveTemp(RowData));
	}

	return Data;
}

FString FDataTableAuditor::SerializeToMarkdown(const FDataTableAuditData& Data)
{
	FString Result;
	Result.Reserve(4096);

	// Header
	Result += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Result += FString::Printf(TEXT("Path: %s\n"), *Data.Path);
	Result += FString::Printf(TEXT("RowStruct: %s\n"), *Data.RowStructName);
	if (!Data.RowStructPath.IsEmpty())
	{
		Result += FString::Printf(TEXT("RowStructPath: %s\n"), *Data.RowStructPath);
	}

	if (!Data.SourceFilePath.IsEmpty())
	{
		Result += FString::Printf(TEXT("Hash: %s\n"), *FAuditFileUtils::ComputeFileHash(Data.SourceFilePath));
	}

	// Numbered column legend
	if (Data.Columns.Num() > 0)
	{
		Result += FString::Printf(TEXT("\n## Columns (%d)\n"), Data.Columns.Num());
		for (int32 i = 0; i < Data.Columns.Num(); ++i)
		{
			Result += FString::Printf(TEXT("%d. %s (%s)\n"), i + 1, *Data.Columns[i].Name, *Data.Columns[i].Type);
		}
	}

	// Per-row sections with numbered values
	if (Data.Rows.Num() > 0)
	{
		Result += FString::Printf(TEXT("\n## Rows (%d)\n"), Data.Rows.Num());

		for (const FDataTableRowData& Row : Data.Rows)
		{
			Result += FString::Printf(TEXT("\n### %s\n"), *Row.RowName);
			for (int32 i = 0; i < Row.Values.Num(); ++i)
			{
				const FString CleanedVal = FathomAuditHelpers::CleanExportedValue(Row.Values[i]);
				if (CleanedVal.IsEmpty() || CleanedVal == TEXT("()"))
				{
					continue;
				}
				Result += FString::Printf(TEXT("%d. %s\n"), i + 1, *CleanedVal);
			}
		}
	}

	return Result;
}

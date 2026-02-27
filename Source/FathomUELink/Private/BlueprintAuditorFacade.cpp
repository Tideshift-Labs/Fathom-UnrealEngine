#include "BlueprintAuditor.h"

// ============================================================================
// FBlueprintAuditor facade: delegates every call to the appropriate domain
// auditor. Preserves backward compatibility for all existing consumers.
// ============================================================================

// --- Blueprint / Graph / Widget ---

FBlueprintAuditData FBlueprintAuditor::GatherBlueprintData(const UBlueprint* BP)
{
	return FBlueprintGraphAuditor::GatherBlueprintData(BP);
}

FGraphAuditData FBlueprintAuditor::GatherGraphData(const UEdGraph* Graph)
{
	return FBlueprintGraphAuditor::GatherGraphData(Graph);
}

FWidgetAuditData FBlueprintAuditor::GatherWidgetData(UWidget* Widget)
{
	return FBlueprintGraphAuditor::GatherWidgetData(Widget);
}

FString FBlueprintAuditor::SerializeToMarkdown(const FBlueprintAuditData& Data)
{
	return FBlueprintGraphAuditor::SerializeToMarkdown(Data);
}

FString FBlueprintAuditor::SerializeGraphToMarkdown(const FGraphAuditData& Data, const FString& Prefix)
{
	return FBlueprintGraphAuditor::SerializeGraphToMarkdown(Data, Prefix);
}

FString FBlueprintAuditor::SerializeWidgetToMarkdown(const FWidgetAuditData& Data, int32 Indent)
{
	return FBlueprintGraphAuditor::SerializeWidgetToMarkdown(Data, Indent);
}

// --- DataTable ---

FDataTableAuditData FBlueprintAuditor::GatherDataTableData(const UDataTable* DataTable)
{
	return FDataTableAuditor::GatherData(DataTable);
}

FString FBlueprintAuditor::SerializeDataTableToMarkdown(const FDataTableAuditData& Data)
{
	return FDataTableAuditor::SerializeToMarkdown(Data);
}

// --- DataAsset ---

FDataAssetAuditData FBlueprintAuditor::GatherDataAssetData(const UDataAsset* Asset)
{
	return FDataAssetAuditor::GatherData(Asset);
}

FString FBlueprintAuditor::SerializeDataAssetToMarkdown(const FDataAssetAuditData& Data)
{
	return FDataAssetAuditor::SerializeToMarkdown(Data);
}

// --- UserDefinedStruct ---

FUserDefinedStructAuditData FBlueprintAuditor::GatherUserDefinedStructData(const UUserDefinedStruct* Struct)
{
	return FUserDefinedStructAuditor::GatherData(Struct);
}

FString FBlueprintAuditor::SerializeUserDefinedStructToMarkdown(const FUserDefinedStructAuditData& Data)
{
	return FUserDefinedStructAuditor::SerializeToMarkdown(Data);
}

// --- ControlRig ---

FControlRigAuditData FBlueprintAuditor::GatherControlRigData(const UControlRigBlueprint* CRBP)
{
	return FControlRigAuditor::GatherData(CRBP);
}

FString FBlueprintAuditor::SerializeControlRigToMarkdown(const FControlRigAuditData& Data)
{
	return FControlRigAuditor::SerializeToMarkdown(Data);
}

// --- Legacy synchronous API ---

FString FBlueprintAuditor::AuditBlueprint(const UBlueprint* BP)
{
	return FBlueprintGraphAuditor::SerializeToMarkdown(FBlueprintGraphAuditor::GatherBlueprintData(BP));
}

FString FBlueprintAuditor::AuditGraph(const UEdGraph* Graph)
{
	return FBlueprintGraphAuditor::SerializeGraphToMarkdown(FBlueprintGraphAuditor::GatherGraphData(Graph), TEXT("EventGraph"));
}

FString FBlueprintAuditor::AuditWidget(UWidget* Widget)
{
	return FBlueprintGraphAuditor::SerializeWidgetToMarkdown(FBlueprintGraphAuditor::GatherWidgetData(Widget));
}

// --- Utilities ---

FString FBlueprintAuditor::GetVariableTypeString(const FEdGraphPinType& PinType)
{
	return FAuditFileUtils::GetVariableTypeString(PinType);
}

FString FBlueprintAuditor::GetAuditBaseDir()
{
	return FAuditFileUtils::GetAuditBaseDir();
}

FString FBlueprintAuditor::GetAuditOutputPath(const UBlueprint* BP)
{
	return FAuditFileUtils::GetAuditOutputPath(BP);
}

FString FBlueprintAuditor::GetAuditOutputPath(const FString& PackageName)
{
	return FAuditFileUtils::GetAuditOutputPath(PackageName);
}

bool FBlueprintAuditor::DeleteAuditFile(const FString& FilePath)
{
	return FAuditFileUtils::DeleteAuditFile(FilePath);
}

FString FBlueprintAuditor::GetSourceFilePath(const FString& PackageName)
{
	return FAuditFileUtils::GetSourceFilePath(PackageName);
}

FString FBlueprintAuditor::ComputeFileHash(const FString& FilePath)
{
	return FAuditFileUtils::ComputeFileHash(FilePath);
}

bool FBlueprintAuditor::WriteAuditFile(const FString& Content, const FString& OutputPath)
{
	return FAuditFileUtils::WriteAuditFile(Content, OutputPath);
}

bool FBlueprintAuditor::IsSupportedBlueprintClass(const FTopLevelAssetPath& ClassPath)
{
	return FAuditFileUtils::IsSupportedBlueprintClass(ClassPath);
}

#include "BlueprintAuditor.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"

DEFINE_LOG_CATEGORY(LogBlueprintAudit);

TSharedPtr<FJsonObject> FBlueprintAuditor::AuditBlueprint(const UBlueprint* BP)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

	// --- Metadata ---
	Result->SetStringField(TEXT("Name"), BP->GetName());
	Result->SetStringField(TEXT("Path"), BP->GetPathName());
	Result->SetStringField(TEXT("ParentClass"),
		BP->ParentClass ? BP->ParentClass->GetPathName() : TEXT("None"));
	Result->SetStringField(TEXT("BlueprintType"),
		StaticEnum<EBlueprintType>()->GetNameStringByValue(static_cast<int64>(BP->BlueprintType)));

	// --- Source file hash (for stale detection) ---
	const FString SourcePath = GetSourceFilePath(BP->GetOutermost()->GetName());
	if (!SourcePath.IsEmpty())
	{
		Result->SetStringField(TEXT("SourceFileHash"), ComputeFileHash(SourcePath));
	}

	UE_LOG(LogBlueprintAudit, Display, TEXT("  %s  (Parent: %s)"),
		*BP->GetName(), BP->ParentClass ? *BP->ParentClass->GetName() : TEXT("None"));

	// --- Variables ---
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShareable(new FJsonObject());
		VarObj->SetStringField(TEXT("Name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("Type"), GetVariableTypeString(Var.VarType));
		VarObj->SetStringField(TEXT("Category"), Var.Category.ToString());
		VarObj->SetBoolField(TEXT("InstanceEditable"),
			Var.HasMetaData(FBlueprintMetadata::MD_Private) == false &&
			Var.PropertyFlags & CPF_Edit);
		VarObj->SetBoolField(TEXT("Replicated"),
			(Var.PropertyFlags & CPF_Net) != 0);
		VariablesArray.Add(MakeShareable(new FJsonValueObject(VarObj)));
	}
	Result->SetArrayField(TEXT("Variables"), VariablesArray);

	// --- Property Overrides (CDO Diff) ---
	TArray<TSharedPtr<FJsonValue>> OverridesArray;
	if (UClass* GeneratedClass = BP->GeneratedClass)
	{
		if (UClass* SuperClass = GeneratedClass->GetSuperClass())
		{
			const UObject* CDO = GeneratedClass->GetDefaultObject();
			const UObject* SuperCDO = SuperClass->GetDefaultObject();

			for (TFieldIterator<FProperty> PropIt(GeneratedClass); PropIt; ++PropIt)
			{
				const FProperty* Prop = *PropIt;

				// Skip properties defined in this Blueprint (we only care about inherited property overrides)
				// Accessing a child-only property on the SuperCDO will crash.
				if (Prop->GetOwner<UClass>() == GeneratedClass)
				{
					continue;
				}

				// Skip properties that aren't editable or config-related
				// (We want to capture what the user changed in the Details panel)
				if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_Config | CPF_DisableEditOnInstance))
				{
					continue;
				}

				// Skip Transient properties
				if (Prop->HasAnyPropertyFlags(CPF_Transient))
				{
					continue;
				}

				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
				const void* SuperValuePtr = Prop->ContainerPtrToValuePtr<void>(SuperCDO);

				if (!Prop->Identical(ValuePtr, SuperValuePtr))
				{
					TSharedPtr<FJsonObject> OverrideObj = MakeShareable(new FJsonObject());
					OverrideObj->SetStringField(TEXT("Name"), Prop->GetName());

					FString ValueStr;
					// Use ExportText_InContainer to avoid manual value pointer handling issues
					// Index 0, Container=CDO, Default=nullptr (force full export), Parent=nullptr, Flags=0
					Prop->ExportText_InContainer(0, ValueStr, CDO, nullptr, nullptr, 0);
					OverrideObj->SetStringField(TEXT("Value"), ValueStr);

					OverridesArray.Add(MakeShareable(new FJsonValueObject(OverrideObj)));
				}
			}
		}
	}
	Result->SetArrayField(TEXT("PropertyOverrides"), OverridesArray);

	// --- Interfaces ---
	TArray<TSharedPtr<FJsonValue>> InterfacesArray;
	for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
	{
		if (Interface.Interface)
		{
			InterfacesArray.Add(MakeShareable(new FJsonValueString(Interface.Interface->GetName())));
		}
	}
	Result->SetArrayField(TEXT("Interfaces"), InterfacesArray);

	// --- Components (Actor-based BPs) ---
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	if (BP->SimpleConstructionScript)
	{
		for (const USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass)
			{
				TSharedPtr<FJsonObject> CompObj = MakeShareable(new FJsonObject());
				CompObj->SetStringField(TEXT("Name"), Node->GetVariableName().ToString());
				CompObj->SetStringField(TEXT("Class"), Node->ComponentClass->GetName());
				ComponentsArray.Add(MakeShareable(new FJsonValueObject(CompObj)));
			}
		}
	}
	Result->SetArrayField(TEXT("Components"), ComponentsArray);

	// --- Widget Tree (Widget Blueprints) ---
	if (const UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP))
	{
		if (WidgetBP->WidgetTree && WidgetBP->WidgetTree->RootWidget)
		{
			Result->SetObjectField(TEXT("WidgetTree"), AuditWidget(WidgetBP->WidgetTree->RootWidget));
		}
	}

	// --- Event Graphs (UbergraphPages) ---
	TArray<TSharedPtr<FJsonValue>> EventGraphs;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		EventGraphs.Add(MakeShareable(new FJsonValueObject(AuditGraph(Graph))));
	}
	Result->SetArrayField(TEXT("EventGraphs"), EventGraphs);

	// --- Function Graphs ---
	TArray<TSharedPtr<FJsonValue>> FunctionGraphs;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		FunctionGraphs.Add(MakeShareable(new FJsonValueObject(AuditGraph(Graph))));
	}
	Result->SetArrayField(TEXT("FunctionGraphs"), FunctionGraphs);

	// --- Macro Graphs ---
	TArray<TSharedPtr<FJsonValue>> MacroGraphs;
	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		TSharedPtr<FJsonObject> MacroObj = MakeShareable(new FJsonObject());
		MacroObj->SetStringField(TEXT("Name"), Graph->GetName());
		MacroObj->SetNumberField(TEXT("NodeCount"), Graph->Nodes.Num());
		MacroGraphs.Add(MakeShareable(new FJsonValueObject(MacroObj)));
	}
	Result->SetArrayField(TEXT("MacroGraphs"), MacroGraphs);

	return Result;
}

TSharedPtr<FJsonObject> FBlueprintAuditor::AuditGraph(const UEdGraph* Graph)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("Name"), Graph->GetName());
	Result->SetNumberField(TEXT("TotalNodes"), Graph->Nodes.Num());

	TArray<TSharedPtr<FJsonValue>> Events;
	TArray<TSharedPtr<FJsonValue>> FunctionCalls;
	TSet<FString> VariablesRead;
	TSet<FString> VariablesWritten;
	TArray<TSharedPtr<FJsonValue>> Macros;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		// Check CustomEvent before Event (CustomEvent inherits from Event)
		if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			Events.Add(MakeShareable(new FJsonValueString(
				FString::Printf(TEXT("CustomEvent: %s"), *CustomEvent->CustomFunctionName.ToString()))));
		}
		else if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			Events.Add(MakeShareable(new FJsonValueString(
				EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString())));
		}
		else if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			TSharedPtr<FJsonObject> CallObj = MakeShareable(new FJsonObject());
			const FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();

			FString TargetClass = TEXT("Self");
			if (const UFunction* Func = CallNode->GetTargetFunction())
			{
				if (const UClass* OwnerClass = Func->GetOwnerClass())
				{
					TargetClass = OwnerClass->GetName();
				}
			}

			CallObj->SetStringField(TEXT("Function"), FuncName);
			CallObj->SetStringField(TEXT("Target"), TargetClass);
			FunctionCalls.Add(MakeShareable(new FJsonValueObject(CallObj)));
		}
		else if (const UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
		{
			VariablesRead.Add(GetNode->GetVarName().ToString());
		}
		else if (const UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
		{
			VariablesWritten.Add(SetNode->GetVarName().ToString());
		}
		else if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
		{
			const FString MacroName = MacroNode->GetMacroGraph()
				? MacroNode->GetMacroGraph()->GetName()
				: TEXT("Unknown");
			Macros.Add(MakeShareable(new FJsonValueString(MacroName)));
		}
	}

	Result->SetArrayField(TEXT("Events"), Events);
	Result->SetArrayField(TEXT("FunctionCalls"), FunctionCalls);

	// Convert TSet to JSON arrays
	TArray<TSharedPtr<FJsonValue>> VarsReadArr;
	for (const FString& Var : VariablesRead)
	{
		VarsReadArr.Add(MakeShareable(new FJsonValueString(Var)));
	}
	Result->SetArrayField(TEXT("VariablesRead"), VarsReadArr);

	TArray<TSharedPtr<FJsonValue>> VarsWrittenArr;
	for (const FString& Var : VariablesWritten)
	{
		VarsWrittenArr.Add(MakeShareable(new FJsonValueString(Var)));
	}
	Result->SetArrayField(TEXT("VariablesWritten"), VarsWrittenArr);

	Result->SetArrayField(TEXT("MacroInstances"), Macros);

	return Result;
}

FString FBlueprintAuditor::GetVariableTypeString(const FEdGraphPinType& PinType)
{
	FString TypeStr = PinType.PinCategory.ToString();

	if (PinType.PinSubCategoryObject.IsValid())
	{
		TypeStr = PinType.PinSubCategoryObject->GetName();
	}

	switch (PinType.ContainerType)
	{
	case EPinContainerType::Array:
		TypeStr = FString::Printf(TEXT("Array<%s>"), *TypeStr);
		break;
	case EPinContainerType::Set:
		TypeStr = FString::Printf(TEXT("Set<%s>"), *TypeStr);
		break;
	case EPinContainerType::Map:
		{
			FString ValueType = TEXT("?");
			if (PinType.PinValueType.TerminalSubCategoryObject.IsValid())
			{
				ValueType = PinType.PinValueType.TerminalSubCategoryObject->GetName();
			}
			else if (!PinType.PinValueType.TerminalCategory.IsNone())
			{
				ValueType = PinType.PinValueType.TerminalCategory.ToString();
			}
			TypeStr = FString::Printf(TEXT("Map<%s, %s>"), *TypeStr, *ValueType);
		}
		break;
	default:
		break;
	}

	return TypeStr;
}

FString FBlueprintAuditor::GetAuditOutputPath(const UBlueprint* BP)
{
	return GetAuditOutputPath(BP->GetOutermost()->GetName());
}

FString FBlueprintAuditor::GetAuditOutputPath(const FString& PackageName)
{
	// Convert package path like /Game/UI/Widgets/WBP_Foo to relative path UI/Widgets/WBP_Foo
	FString RelativePath = PackageName;

	const FString GamePrefix = TEXT("/Game/");
	if (RelativePath.StartsWith(GamePrefix))
	{
		RelativePath.RightChopInline(GamePrefix.Len());
	}

	// Build: <ProjectDir>/Saved/Audit/Blueprints/<relative_path>.json
	return FPaths::ProjectDir() / TEXT("Saved") / TEXT("Audit") / TEXT("Blueprints") / RelativePath + TEXT(".json");
}

FString FBlueprintAuditor::GetSourceFilePath(const FString& PackageName)
{
	FString FilePath;
	if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, FilePath, FPackageName::GetAssetPackageExtension()))
	{
		return FPaths::ConvertRelativePathToFull(FilePath);
	}
	return FString();
}

FString FBlueprintAuditor::ComputeFileHash(const FString& FilePath)
{
	const FMD5Hash Hash = FMD5Hash::HashFile(*FilePath);
	if (Hash.IsValid())
	{
		return LexToString(Hash);
	}
	return FString();
}

bool FBlueprintAuditor::WriteAuditJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& OutputPath)
{
	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

	if (FFileHelper::SaveStringToFile(OutputString, *OutputPath))
	{
		UE_LOG(LogBlueprintAudit, Display, TEXT("Audit saved to: %s"), *OutputPath);
		return true;
	}

	UE_LOG(LogBlueprintAudit, Error, TEXT("Failed to write: %s"), *OutputPath);
	return false;
}

TSharedPtr<FJsonObject> FBlueprintAuditor::AuditWidget(UWidget* Widget)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
	if (!Widget)
	{
		return Result;
	}

	Result->SetStringField(TEXT("Name"), Widget->GetName());
	Result->SetStringField(TEXT("Class"), Widget->GetClass()->GetName());
	Result->SetBoolField(TEXT("IsVariable"), Widget->bIsVariable);

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			if (UWidget* Child = Panel->GetChildAt(i))
			{
				ChildrenArray.Add(MakeShareable(new FJsonValueObject(AuditWidget(Child))));
			}
		}
		Result->SetArrayField(TEXT("Children"), ChildrenArray);
	}

	return Result;
}

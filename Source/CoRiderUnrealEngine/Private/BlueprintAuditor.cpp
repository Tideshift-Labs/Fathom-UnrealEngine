#include "BlueprintAuditor.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
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

DEFINE_LOG_CATEGORY(LogCoRider);

// ============================================================================
// Game-thread gather functions (read UObject pointers, populate POD structs)
// ============================================================================

FBlueprintAuditData FBlueprintAuditor::GatherBlueprintData(const UBlueprint* BP)
{
	FBlueprintAuditData Data;

	// --- Metadata ---
	Data.Name = BP->GetName();
	Data.Path = BP->GetPathName();
	Data.PackageName = BP->GetOutermost()->GetName();
	Data.ParentClass = BP->ParentClass ? BP->ParentClass->GetPathName() : TEXT("None");
	Data.BlueprintType = StaticEnum<EBlueprintType>()->GetNameStringByValue(static_cast<int64>(BP->BlueprintType));

	// Store the source file path so the hash can be computed on a background thread
	Data.SourceFilePath = GetSourceFilePath(Data.PackageName);
	Data.OutputPath = GetAuditOutputPath(Data.PackageName);

	UE_LOG(LogCoRider, Verbose, TEXT("CoRider: Gathering data for %s (Parent: %s)"),
		*Data.Name, BP->ParentClass ? *BP->ParentClass->GetName() : TEXT("None"));

	// --- Variables ---
	Data.Variables.Reserve(BP->NewVariables.Num());
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		FVariableAuditData VarData;
		VarData.Name = Var.VarName.ToString();
		VarData.Type = GetVariableTypeString(Var.VarType);
		VarData.Category = Var.Category.ToString();
		VarData.bInstanceEditable =
			Var.HasMetaData(FBlueprintMetadata::MD_Private) == false &&
			(Var.PropertyFlags & CPF_Edit) != 0;
		VarData.bReplicated = (Var.PropertyFlags & CPF_Net) != 0;
		Data.Variables.Add(MoveTemp(VarData));
	}

	// --- Property Overrides (CDO Diff) ---
	if (UClass* GeneratedClass = BP->GeneratedClass)
	{
		if (UClass* SuperClass = GeneratedClass->GetSuperClass())
		{
			const UObject* CDO = GeneratedClass->GetDefaultObject();
			const UObject* SuperCDO = SuperClass->GetDefaultObject();

			for (TFieldIterator<FProperty> PropIt(GeneratedClass); PropIt; ++PropIt)
			{
				const FProperty* Prop = *PropIt;

				if (Prop->GetOwner<UClass>() == GeneratedClass)
				{
					continue;
				}

				if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_Config | CPF_DisableEditOnInstance))
				{
					continue;
				}

				if (Prop->HasAnyPropertyFlags(CPF_Transient))
				{
					continue;
				}

				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
				const void* SuperValuePtr = Prop->ContainerPtrToValuePtr<void>(SuperCDO);

				if (!Prop->Identical(ValuePtr, SuperValuePtr))
				{
					FPropertyOverrideData Override;
					Override.Name = Prop->GetName();
					Prop->ExportText_InContainer(0, Override.Value, CDO, nullptr, nullptr, 0);
					Data.PropertyOverrides.Add(MoveTemp(Override));
				}
			}
		}
	}

	// --- Interfaces ---
	for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
	{
		if (Interface.Interface)
		{
			Data.Interfaces.Add(Interface.Interface->GetName());
		}
	}

	// --- Components (Actor-based BPs) ---
	if (BP->SimpleConstructionScript)
	{
		for (const USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass)
			{
				FComponentAuditData CompData;
				CompData.Name = Node->GetVariableName().ToString();
				CompData.Class = Node->ComponentClass->GetName();
				Data.Components.Add(MoveTemp(CompData));
			}
		}
	}

	// --- Timelines ---
	for (const UTimelineTemplate* Timeline : BP->Timelines)
	{
		if (!Timeline) continue;

		FTimelineAuditData TLData;
		TLData.Name = Timeline->GetName();
		TLData.Length = Timeline->TimelineLength;
		TLData.bLooping = Timeline->bLoop;
		TLData.bAutoPlay = Timeline->bAutoPlay;
		TLData.FloatTrackCount = Timeline->FloatTracks.Num();
		TLData.VectorTrackCount = Timeline->VectorTracks.Num();
		TLData.LinearColorTrackCount = Timeline->LinearColorTracks.Num();
		TLData.EventTrackCount = Timeline->EventTracks.Num();
		Data.Timelines.Add(MoveTemp(TLData));
	}

	// --- Widget Tree (Widget Blueprints) ---
	if (const UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP))
	{
		if (WidgetBP->WidgetTree && WidgetBP->WidgetTree->RootWidget)
		{
			Data.WidgetTree = GatherWidgetData(WidgetBP->WidgetTree->RootWidget);
		}
	}

	// --- Event Graphs (UbergraphPages) ---
	Data.EventGraphs.Reserve(BP->UbergraphPages.Num());
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		Data.EventGraphs.Add(GatherGraphData(Graph));
	}

	// --- Function Graphs ---
	Data.FunctionGraphs.Reserve(BP->FunctionGraphs.Num());
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		Data.FunctionGraphs.Add(GatherGraphData(Graph));
	}

	// --- Macro Graphs ---
	Data.MacroGraphs.Reserve(BP->MacroGraphs.Num());
	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		FMacroGraphAuditData MacroData;
		MacroData.Name = Graph->GetName();
		MacroData.NodeCount = Graph->Nodes.Num();
		Data.MacroGraphs.Add(MoveTemp(MacroData));
	}

	return Data;
}

FGraphAuditData FBlueprintAuditor::GatherGraphData(const UEdGraph* Graph)
{
	FGraphAuditData Data;
	Data.Name = Graph->GetName();
	Data.TotalNodes = Graph->Nodes.Num();

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		// Check CustomEvent before Event (CustomEvent inherits from Event)
		if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			Data.Events.Add(FString::Printf(TEXT("CustomEvent: %s"), *CustomEvent->CustomFunctionName.ToString()));
		}
		else if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			Data.Events.Add(EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
		}
		else if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			FCallFunctionAuditData CallData;
			CallData.FunctionName = CallNode->FunctionReference.GetMemberName().ToString();

			CallData.TargetClass = TEXT("Self");
			const UFunction* Func = CallNode->GetTargetFunction();
			if (Func)
			{
				if (const UClass* OwnerClass = Func->GetOwnerClass())
				{
					CallData.TargetClass = OwnerClass->GetName();
				}
			}

			CallData.bIsNative = Func && Func->IsNative();

			// Capture hardcoded (literal) input pin values
			for (const UEdGraphPin* Pin : CallNode->Pins)
			{
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->LinkedTo.Num() > 0) continue;
				if (Pin->DefaultValue.IsEmpty()) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;

				FDefaultInputData InputData;
				InputData.Name = Pin->PinName.ToString();
				InputData.Value = Pin->DefaultValue;
				CallData.DefaultInputs.Add(MoveTemp(InputData));
			}

			Data.FunctionCalls.Add(MoveTemp(CallData));
		}
		else if (const UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
		{
			Data.VariablesRead.Add(GetNode->GetVarName().ToString());
		}
		else if (const UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
		{
			Data.VariablesWritten.Add(SetNode->GetVarName().ToString());
		}
		else if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
		{
			const FString MacroName = MacroNode->GetMacroGraph()
				? MacroNode->GetMacroGraph()->GetName()
				: TEXT("Unknown");
			Data.MacroInstances.Add(MacroName);
		}
	}

	return Data;
}

FWidgetAuditData FBlueprintAuditor::GatherWidgetData(UWidget* Widget)
{
	FWidgetAuditData Data;
	if (!Widget)
	{
		return Data;
	}

	Data.Name = Widget->GetName();
	Data.Class = Widget->GetClass()->GetName();
	Data.bIsVariable = Widget->bIsVariable;

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		Data.Children.Reserve(Panel->GetChildrenCount());
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			if (UWidget* Child = Panel->GetChildAt(i))
			{
				Data.Children.Add(GatherWidgetData(Child));
			}
		}
	}

	return Data;
}

// ============================================================================
// Thread-safe serialize functions (POD structs to JSON, no UObject access)
// ============================================================================

TSharedPtr<FJsonObject> FBlueprintAuditor::SerializeToJson(const FBlueprintAuditData& Data)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

	// --- Metadata ---
	Result->SetStringField(TEXT("Name"), Data.Name);
	Result->SetStringField(TEXT("Path"), Data.Path);
	Result->SetStringField(TEXT("ParentClass"), Data.ParentClass);
	Result->SetStringField(TEXT("BlueprintType"), Data.BlueprintType);

	// --- Source file hash (computed here on the background thread) ---
	if (!Data.SourceFilePath.IsEmpty())
	{
		Result->SetStringField(TEXT("SourceFileHash"), ComputeFileHash(Data.SourceFilePath));
	}

	// --- Variables ---
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	VariablesArray.Reserve(Data.Variables.Num());
	for (const FVariableAuditData& Var : Data.Variables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShareable(new FJsonObject());
		VarObj->SetStringField(TEXT("Name"), Var.Name);
		VarObj->SetStringField(TEXT("Type"), Var.Type);
		VarObj->SetStringField(TEXT("Category"), Var.Category);
		VarObj->SetBoolField(TEXT("InstanceEditable"), Var.bInstanceEditable);
		VarObj->SetBoolField(TEXT("Replicated"), Var.bReplicated);
		VariablesArray.Add(MakeShareable(new FJsonValueObject(VarObj)));
	}
	Result->SetArrayField(TEXT("Variables"), VariablesArray);

	// --- Property Overrides ---
	TArray<TSharedPtr<FJsonValue>> OverridesArray;
	OverridesArray.Reserve(Data.PropertyOverrides.Num());
	for (const FPropertyOverrideData& Override : Data.PropertyOverrides)
	{
		TSharedPtr<FJsonObject> OverrideObj = MakeShareable(new FJsonObject());
		OverrideObj->SetStringField(TEXT("Name"), Override.Name);
		OverrideObj->SetStringField(TEXT("Value"), Override.Value);
		OverridesArray.Add(MakeShareable(new FJsonValueObject(OverrideObj)));
	}
	Result->SetArrayField(TEXT("PropertyOverrides"), OverridesArray);

	// --- Interfaces ---
	TArray<TSharedPtr<FJsonValue>> InterfacesArray;
	InterfacesArray.Reserve(Data.Interfaces.Num());
	for (const FString& Iface : Data.Interfaces)
	{
		InterfacesArray.Add(MakeShareable(new FJsonValueString(Iface)));
	}
	Result->SetArrayField(TEXT("Interfaces"), InterfacesArray);

	// --- Components ---
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	ComponentsArray.Reserve(Data.Components.Num());
	for (const FComponentAuditData& Comp : Data.Components)
	{
		TSharedPtr<FJsonObject> CompObj = MakeShareable(new FJsonObject());
		CompObj->SetStringField(TEXT("Name"), Comp.Name);
		CompObj->SetStringField(TEXT("Class"), Comp.Class);
		ComponentsArray.Add(MakeShareable(new FJsonValueObject(CompObj)));
	}
	Result->SetArrayField(TEXT("Components"), ComponentsArray);

	// --- Timelines ---
	TArray<TSharedPtr<FJsonValue>> TimelinesArray;
	TimelinesArray.Reserve(Data.Timelines.Num());
	for (const FTimelineAuditData& TL : Data.Timelines)
	{
		TSharedPtr<FJsonObject> TLObj = MakeShareable(new FJsonObject());
		TLObj->SetStringField(TEXT("Name"), TL.Name);
		TLObj->SetNumberField(TEXT("Length"), TL.Length);
		TLObj->SetBoolField(TEXT("Looping"), TL.bLooping);
		TLObj->SetBoolField(TEXT("AutoPlay"), TL.bAutoPlay);
		TLObj->SetNumberField(TEXT("FloatTrackCount"), TL.FloatTrackCount);
		TLObj->SetNumberField(TEXT("VectorTrackCount"), TL.VectorTrackCount);
		TLObj->SetNumberField(TEXT("LinearColorTrackCount"), TL.LinearColorTrackCount);
		TLObj->SetNumberField(TEXT("EventTrackCount"), TL.EventTrackCount);
		TimelinesArray.Add(MakeShareable(new FJsonValueObject(TLObj)));
	}
	Result->SetArrayField(TEXT("Timelines"), TimelinesArray);

	// --- Widget Tree ---
	if (Data.WidgetTree.IsSet())
	{
		Result->SetObjectField(TEXT("WidgetTree"), SerializeWidgetToJson(Data.WidgetTree.GetValue()));
	}

	// --- Event Graphs ---
	TArray<TSharedPtr<FJsonValue>> EventGraphs;
	EventGraphs.Reserve(Data.EventGraphs.Num());
	for (const FGraphAuditData& Graph : Data.EventGraphs)
	{
		EventGraphs.Add(MakeShareable(new FJsonValueObject(SerializeGraphToJson(Graph))));
	}
	Result->SetArrayField(TEXT("EventGraphs"), EventGraphs);

	// --- Function Graphs ---
	TArray<TSharedPtr<FJsonValue>> FunctionGraphs;
	FunctionGraphs.Reserve(Data.FunctionGraphs.Num());
	for (const FGraphAuditData& Graph : Data.FunctionGraphs)
	{
		FunctionGraphs.Add(MakeShareable(new FJsonValueObject(SerializeGraphToJson(Graph))));
	}
	Result->SetArrayField(TEXT("FunctionGraphs"), FunctionGraphs);

	// --- Macro Graphs ---
	TArray<TSharedPtr<FJsonValue>> MacroGraphs;
	MacroGraphs.Reserve(Data.MacroGraphs.Num());
	for (const FMacroGraphAuditData& Macro : Data.MacroGraphs)
	{
		TSharedPtr<FJsonObject> MacroObj = MakeShareable(new FJsonObject());
		MacroObj->SetStringField(TEXT("Name"), Macro.Name);
		MacroObj->SetNumberField(TEXT("NodeCount"), Macro.NodeCount);
		MacroGraphs.Add(MakeShareable(new FJsonValueObject(MacroObj)));
	}
	Result->SetArrayField(TEXT("MacroGraphs"), MacroGraphs);

	return Result;
}

TSharedPtr<FJsonObject> FBlueprintAuditor::SerializeGraphToJson(const FGraphAuditData& Data)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("Name"), Data.Name);
	Result->SetNumberField(TEXT("TotalNodes"), Data.TotalNodes);

	// Events
	TArray<TSharedPtr<FJsonValue>> Events;
	Events.Reserve(Data.Events.Num());
	for (const FString& Evt : Data.Events)
	{
		Events.Add(MakeShareable(new FJsonValueString(Evt)));
	}
	Result->SetArrayField(TEXT("Events"), Events);

	// Function Calls
	TArray<TSharedPtr<FJsonValue>> FunctionCalls;
	FunctionCalls.Reserve(Data.FunctionCalls.Num());
	for (const FCallFunctionAuditData& Call : Data.FunctionCalls)
	{
		TSharedPtr<FJsonObject> CallObj = MakeShareable(new FJsonObject());
		CallObj->SetStringField(TEXT("Function"), Call.FunctionName);
		CallObj->SetStringField(TEXT("Target"), Call.TargetClass);
		CallObj->SetBoolField(TEXT("IsNative"), Call.bIsNative);

		if (Call.DefaultInputs.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> DefaultInputs;
			DefaultInputs.Reserve(Call.DefaultInputs.Num());
			for (const FDefaultInputData& Input : Call.DefaultInputs)
			{
				TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject());
				PinObj->SetStringField(TEXT("Name"), Input.Name);
				PinObj->SetStringField(TEXT("Value"), Input.Value);
				DefaultInputs.Add(MakeShareable(new FJsonValueObject(PinObj)));
			}
			CallObj->SetArrayField(TEXT("DefaultInputs"), DefaultInputs);
		}

		FunctionCalls.Add(MakeShareable(new FJsonValueObject(CallObj)));
	}
	Result->SetArrayField(TEXT("FunctionCalls"), FunctionCalls);

	// Variables Read
	TArray<TSharedPtr<FJsonValue>> VarsReadArr;
	VarsReadArr.Reserve(Data.VariablesRead.Num());
	for (const FString& Var : Data.VariablesRead)
	{
		VarsReadArr.Add(MakeShareable(new FJsonValueString(Var)));
	}
	Result->SetArrayField(TEXT("VariablesRead"), VarsReadArr);

	// Variables Written
	TArray<TSharedPtr<FJsonValue>> VarsWrittenArr;
	VarsWrittenArr.Reserve(Data.VariablesWritten.Num());
	for (const FString& Var : Data.VariablesWritten)
	{
		VarsWrittenArr.Add(MakeShareable(new FJsonValueString(Var)));
	}
	Result->SetArrayField(TEXT("VariablesWritten"), VarsWrittenArr);

	// Macro Instances
	TArray<TSharedPtr<FJsonValue>> Macros;
	Macros.Reserve(Data.MacroInstances.Num());
	for (const FString& MacroName : Data.MacroInstances)
	{
		Macros.Add(MakeShareable(new FJsonValueString(MacroName)));
	}
	Result->SetArrayField(TEXT("MacroInstances"), Macros);

	return Result;
}

TSharedPtr<FJsonObject> FBlueprintAuditor::SerializeWidgetToJson(const FWidgetAuditData& Data)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

	Result->SetStringField(TEXT("Name"), Data.Name);
	Result->SetStringField(TEXT("Class"), Data.Class);
	Result->SetBoolField(TEXT("IsVariable"), Data.bIsVariable);

	if (Data.Children.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		ChildrenArray.Reserve(Data.Children.Num());
		for (const FWidgetAuditData& Child : Data.Children)
		{
			ChildrenArray.Add(MakeShareable(new FJsonValueObject(SerializeWidgetToJson(Child))));
		}
		Result->SetArrayField(TEXT("Children"), ChildrenArray);
	}

	return Result;
}

// ============================================================================
// Legacy synchronous API (thin wrappers over Gather + Serialize)
// ============================================================================

TSharedPtr<FJsonObject> FBlueprintAuditor::AuditBlueprint(const UBlueprint* BP)
{
	return SerializeToJson(GatherBlueprintData(BP));
}

TSharedPtr<FJsonObject> FBlueprintAuditor::AuditGraph(const UEdGraph* Graph)
{
	return SerializeGraphToJson(GatherGraphData(Graph));
}

TSharedPtr<FJsonObject> FBlueprintAuditor::AuditWidget(UWidget* Widget)
{
	return SerializeWidgetToJson(GatherWidgetData(Widget));
}

// ============================================================================
// Utility functions (unchanged)
// ============================================================================

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

FString FBlueprintAuditor::GetAuditBaseDir()
{
	const FString VersionDir = FString::Printf(TEXT("v%d"), AuditSchemaVersion);
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Saved") / TEXT("Audit") / VersionDir / TEXT("Blueprints"));
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

	return GetAuditBaseDir() / RelativePath + TEXT(".json");
}

bool FBlueprintAuditor::DeleteAuditJson(const FString& JsonPath)
{
	IFileManager& FM = IFileManager::Get();
	if (!FM.FileExists(*JsonPath))
	{
		return true;
	}

	if (FM.Delete(*JsonPath))
	{
		UE_LOG(LogCoRider, Display, TEXT("CoRider: Deleted audit JSON %s"), *JsonPath);
		return true;
	}

	UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to delete audit JSON %s"), *JsonPath);
	return false;
}

FString FBlueprintAuditor::GetSourceFilePath(const FString& PackageName)
{
	FString FilePath;
	if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, FilePath, FPackageName::GetAssetPackageExtension()))
	{
		return FPaths::ConvertRelativePathToFull(FilePath);
	}
	UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to resolve source path for %s"), *PackageName);
	return FString();
}

FString FBlueprintAuditor::ComputeFileHash(const FString& FilePath)
{
	const FMD5Hash Hash = FMD5Hash::HashFile(*FilePath);
	if (Hash.IsValid())
	{
		return LexToString(Hash);
	}
	UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to compute hash for %s"), *FilePath);
	return FString();
}

bool FBlueprintAuditor::WriteAuditJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& OutputPath)
{
	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

	if (FFileHelper::SaveStringToFile(OutputString, *OutputPath))
	{
		UE_LOG(LogCoRider, Verbose, TEXT("CoRider: Audit saved to %s"), *OutputPath);
		return true;
	}

	UE_LOG(LogCoRider, Error, TEXT("CoRider: Failed to write %s"), *OutputPath);
	return false;
}

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
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Knot.h"
#include "K2Node_Timeline.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
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
// Internal helpers
// ============================================================================

namespace
{
	/** Follow pin connections through UK2Node_Knot reroute nodes to find real endpoints. */
	TArray<UEdGraphPin*> TraceThroughKnots(UEdGraphPin* Pin)
	{
		TArray<UEdGraphPin*> Result;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			UEdGraphNode* Owner = Linked->GetOwningNode();
			if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Owner))
			{
				// Find the output pin on the knot and recurse
				for (UEdGraphPin* KnotPin : Knot->Pins)
				{
					if (KnotPin->Direction == Pin->Direction)
					{
						continue;
					}
					Result.Append(TraceThroughKnots(KnotPin));
				}
			}
			else
			{
				Result.Add(Linked);
			}
		}
		return Result;
	}

	/** Returns true if a node has no exec pins (i.e. it is a pure node). */
	bool IsNodePure(const UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return false;
			}
		}
		return true;
	}

	/** Strip SKEL_ prefix and _C suffix from class names. */
	FString CleanClassName(const FString& RawName)
	{
		FString Name = RawName;
		if (Name.StartsWith(TEXT("SKEL_")))
		{
			Name.RightChopInline(5);
		}
		if (Name.EndsWith(TEXT("_C")))
		{
			Name.LeftChopInline(2);
		}
		return Name;
	}
}

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

	// --- Macro Graphs (full topology, same as event/function graphs) ---
	Data.MacroGraphs.Reserve(BP->MacroGraphs.Num());
	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		Data.MacroGraphs.Add(GatherGraphData(Graph));
	}

	return Data;
}

FGraphAuditData FBlueprintAuditor::GatherGraphData(const UEdGraph* Graph)
{
	FGraphAuditData Data;
	Data.Name = Graph->GetName();

	// ---- Pass 1: Build node list ----

	TMap<UEdGraphNode*, int32> NodeIdMap;
	int32 NextId = 0;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		// Skip reroute/knot nodes entirely
		if (Cast<UK2Node_Knot>(Node))
		{
			continue;
		}

		const int32 NodeId = NextId++;
		NodeIdMap.Add(Node, NodeId);

		FNodeAuditData NodeData;
		NodeData.Id = NodeId;
		NodeData.bPure = IsNodePure(Node);

		// Classify node type
		// Check CustomEvent before Event (CustomEvent inherits from Event)
		if (Cast<UK2Node_FunctionEntry>(Node))
		{
			NodeData.Type = TEXT("FunctionEntry");
			NodeData.Name = Data.Name;

			// Extract function input parameters from the entry node's output pins
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->bHidden) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

				FGraphParamData Param;
				Param.Name = Pin->PinName.ToString();
				Param.Type = GetVariableTypeString(Pin->PinType);
				Data.Inputs.Add(MoveTemp(Param));
			}
		}
		else if (Cast<UK2Node_FunctionResult>(Node))
		{
			NodeData.Type = TEXT("FunctionResult");
			NodeData.Name = TEXT("Return");

			// Extract function output parameters from the result node's input pins
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->bHidden) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

				FGraphParamData Param;
				Param.Name = Pin->PinName.ToString();
				Param.Type = GetVariableTypeString(Pin->PinType);
				Data.Outputs.Add(MoveTemp(Param));
			}
		}
		else if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			NodeData.Type = TEXT("CustomEvent");
			NodeData.Name = CustomEvent->CustomFunctionName.ToString();
		}
		else if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			NodeData.Type = TEXT("Event");
			NodeData.Name = EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}
		else if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			NodeData.Type = TEXT("CallFunction");
			NodeData.Name = CallNode->FunctionReference.GetMemberName().ToString();

			NodeData.Target = TEXT("Self");
			const UFunction* Func = CallNode->GetTargetFunction();
			if (Func)
			{
				if (const UClass* OwnerClass = Func->GetOwnerClass())
				{
					NodeData.Target = CleanClassName(OwnerClass->GetName());
				}
				NodeData.bIsNative = Func->IsNative();
				NodeData.bLatent = Func->HasMetaData(TEXT("Latent"));
			}

			// Capture hardcoded (literal) input pin values
			for (const UEdGraphPin* Pin : CallNode->Pins)
			{
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->bHidden) continue;
				if (Pin->LinkedTo.Num() > 0) continue;
				if (Pin->DefaultValue.IsEmpty()) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;

				FDefaultInputData InputData;
				InputData.Name = Pin->PinName.ToString();
				InputData.Value = Pin->DefaultValue;
				NodeData.DefaultInputs.Add(MoveTemp(InputData));
			}
		}
		else if (Cast<UK2Node_IfThenElse>(Node))
		{
			NodeData.Type = TEXT("Branch");
			NodeData.Name = TEXT("Branch");
		}
		else if (Cast<UK2Node_ExecutionSequence>(Node))
		{
			NodeData.Type = TEXT("Sequence");
			NodeData.Name = TEXT("Sequence");
		}
		else if (const UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
		{
			NodeData.Type = TEXT("VariableGet");
			NodeData.Name = GetNode->GetVarName().ToString();
		}
		else if (const UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
		{
			NodeData.Type = TEXT("VariableSet");
			NodeData.Name = SetNode->GetVarName().ToString();
		}
		else if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
		{
			NodeData.Type = TEXT("MacroInstance");
			NodeData.Name = MacroNode->GetMacroGraph()
				? MacroNode->GetMacroGraph()->GetName()
				: TEXT("Unknown");
		}
		else if (Cast<UK2Node_Timeline>(Node))
		{
			NodeData.Type = TEXT("Timeline");
			NodeData.Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}
		else
		{
			NodeData.Type = TEXT("Other");
			NodeData.Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}

		Data.Nodes.Add(MoveTemp(NodeData));
	}

	// ---- Pass 2: Build edges (walk OUTPUT pins only to avoid duplicates) ----

	for (const auto& Pair : NodeIdMap)
	{
		UEdGraphNode* Node = Pair.Key;
		const int32 SourceId = Pair.Value;

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction != EGPD_Output) continue;
			if (Pin->bHidden) continue;
			if (Pin->LinkedTo.Num() == 0) continue;

			const bool bIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			const FString SrcPinName = Pin->PinName.ToString();

			// Resolve through knot/reroute nodes
			TArray<UEdGraphPin*> ResolvedPins = TraceThroughKnots(const_cast<UEdGraphPin*>(Pin));

			for (UEdGraphPin* TargetPin : ResolvedPins)
			{
				UEdGraphNode* TargetNode = TargetPin->GetOwningNode();
				const int32* TargetIdPtr = NodeIdMap.Find(TargetNode);
				if (!TargetIdPtr) continue;

				if (bIsExec)
				{
					FExecEdge Edge;
					Edge.SourceNodeId = SourceId;
					Edge.SourcePinName = SrcPinName;
					Edge.TargetNodeId = *TargetIdPtr;
					Data.ExecFlows.Add(MoveTemp(Edge));
				}
				else
				{
					FDataEdge Edge;
					Edge.SourceNodeId = SourceId;
					Edge.SourcePinName = SrcPinName;
					Edge.TargetNodeId = *TargetIdPtr;
					Edge.TargetPinName = TargetPin->PinName.ToString();
					Data.DataFlows.Add(MoveTemp(Edge));
				}
			}
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

	// --- Macro Graphs (full topology, same format as event/function graphs) ---
	TArray<TSharedPtr<FJsonValue>> MacroGraphs;
	MacroGraphs.Reserve(Data.MacroGraphs.Num());
	for (const FGraphAuditData& Macro : Data.MacroGraphs)
	{
		MacroGraphs.Add(MakeShareable(new FJsonValueObject(SerializeGraphToJson(Macro))));
	}
	Result->SetArrayField(TEXT("MacroGraphs"), MacroGraphs);

	return Result;
}

TSharedPtr<FJsonObject> FBlueprintAuditor::SerializeGraphToJson(const FGraphAuditData& Data)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("Name"), Data.Name);

	// Function/macro signature
	if (Data.Inputs.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InputsArray;
		InputsArray.Reserve(Data.Inputs.Num());
		for (const FGraphParamData& Param : Data.Inputs)
		{
			TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject());
			ParamObj->SetStringField(TEXT("Name"), Param.Name);
			ParamObj->SetStringField(TEXT("Type"), Param.Type);
			InputsArray.Add(MakeShareable(new FJsonValueObject(ParamObj)));
		}
		Result->SetArrayField(TEXT("Inputs"), InputsArray);
	}

	if (Data.Outputs.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> OutputsArray;
		OutputsArray.Reserve(Data.Outputs.Num());
		for (const FGraphParamData& Param : Data.Outputs)
		{
			TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject());
			ParamObj->SetStringField(TEXT("Name"), Param.Name);
			ParamObj->SetStringField(TEXT("Type"), Param.Type);
			OutputsArray.Add(MakeShareable(new FJsonValueObject(ParamObj)));
		}
		Result->SetArrayField(TEXT("Outputs"), OutputsArray);
	}

	// Nodes
	if (Data.Nodes.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NodesArray;
		NodesArray.Reserve(Data.Nodes.Num());
		for (const FNodeAuditData& NodeData : Data.Nodes)
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject());
			NodeObj->SetNumberField(TEXT("Id"), NodeData.Id);
			NodeObj->SetStringField(TEXT("Type"), NodeData.Type);
			NodeObj->SetStringField(TEXT("Name"), NodeData.Name);

			if (!NodeData.Target.IsEmpty())
			{
				NodeObj->SetStringField(TEXT("Target"), NodeData.Target);
			}
			if (!NodeData.bIsNative && NodeData.Type == TEXT("CallFunction"))
			{
				NodeObj->SetBoolField(TEXT("IsNative"), false);
			}
			if (NodeData.bPure)
			{
				NodeObj->SetBoolField(TEXT("Pure"), true);
			}
			if (NodeData.bLatent)
			{
				NodeObj->SetBoolField(TEXT("Latent"), true);
			}
			if (NodeData.DefaultInputs.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> DefaultInputs;
				DefaultInputs.Reserve(NodeData.DefaultInputs.Num());
				for (const FDefaultInputData& Input : NodeData.DefaultInputs)
				{
					TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject());
					PinObj->SetStringField(TEXT("Name"), Input.Name);
					PinObj->SetStringField(TEXT("Value"), Input.Value);
					DefaultInputs.Add(MakeShareable(new FJsonValueObject(PinObj)));
				}
				NodeObj->SetArrayField(TEXT("DefaultInputs"), DefaultInputs);
			}

			NodesArray.Add(MakeShareable(new FJsonValueObject(NodeObj)));
		}
		Result->SetArrayField(TEXT("Nodes"), NodesArray);
	}

	// V3 Topology: ExecFlows
	if (Data.ExecFlows.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ExecArray;
		ExecArray.Reserve(Data.ExecFlows.Num());
		for (const FExecEdge& Edge : Data.ExecFlows)
		{
			TSharedPtr<FJsonObject> EdgeObj = MakeShareable(new FJsonObject());
			EdgeObj->SetNumberField(TEXT("Src"), Edge.SourceNodeId);
			EdgeObj->SetStringField(TEXT("SrcPin"), Edge.SourcePinName);
			EdgeObj->SetNumberField(TEXT("Dst"), Edge.TargetNodeId);
			ExecArray.Add(MakeShareable(new FJsonValueObject(EdgeObj)));
		}
		Result->SetArrayField(TEXT("ExecFlows"), ExecArray);
	}

	// V3 Topology: DataFlows
	if (Data.DataFlows.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> DataArray;
		DataArray.Reserve(Data.DataFlows.Num());
		for (const FDataEdge& Edge : Data.DataFlows)
		{
			TSharedPtr<FJsonObject> EdgeObj = MakeShareable(new FJsonObject());
			EdgeObj->SetNumberField(TEXT("Src"), Edge.SourceNodeId);
			EdgeObj->SetStringField(TEXT("SrcPin"), Edge.SourcePinName);
			EdgeObj->SetNumberField(TEXT("Dst"), Edge.TargetNodeId);
			EdgeObj->SetStringField(TEXT("DstPin"), Edge.TargetPinName);
			DataArray.Add(MakeShareable(new FJsonValueObject(EdgeObj)));
		}
		Result->SetArrayField(TEXT("DataFlows"), DataArray);
	}

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

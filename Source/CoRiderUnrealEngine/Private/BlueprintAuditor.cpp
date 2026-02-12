#include "BlueprintAuditor.h"

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
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlotInterface.h"
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

	// Named slot content (template widgets / user widgets with slots)
	if (INamedSlotInterface* SlotHost = Cast<INamedSlotInterface>(Widget))
	{
		TArray<FName> SlotNames;
		SlotHost->GetSlotNames(SlotNames);
		for (const FName& SlotName : SlotNames)
		{
			if (UWidget* Content = SlotHost->GetContentForSlot(SlotName))
			{
				FWidgetAuditData SlotData = GatherWidgetData(Content);
				SlotData.SlotName = SlotName.ToString();
				Data.Children.Add(MoveTemp(SlotData));
			}
		}
	}

	return Data;
}

// ============================================================================
// Thread-safe serialize functions (POD structs to Markdown, no UObject access)
// ============================================================================

FString FBlueprintAuditor::SerializeToMarkdown(const FBlueprintAuditData& Data)
{
	FString Result;
	Result.Reserve(4096);

	// --- Header block ---
	Result += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Result += FString::Printf(TEXT("Path: %s\n"), *Data.Path);
	Result += FString::Printf(TEXT("Parent: %s\n"), *Data.ParentClass);
	Result += FString::Printf(TEXT("Type: %s\n"), *Data.BlueprintType);

	if (!Data.SourceFilePath.IsEmpty())
	{
		Result += FString::Printf(TEXT("Hash: %s\n"), *ComputeFileHash(Data.SourceFilePath));
	}

	// --- Variables ---
	if (Data.Variables.Num() > 0)
	{
		Result += TEXT("\n## Variables\n");
		Result += TEXT("| Name | Type | Category | Editable | Replicated |\n");
		Result += TEXT("|------|------|----------|----------|------------|\n");
		for (const FVariableAuditData& Var : Data.Variables)
		{
			Result += FString::Printf(TEXT("| %s | %s | %s | %s | %s |\n"),
				*Var.Name, *Var.Type, *Var.Category,
				Var.bInstanceEditable ? TEXT("Yes") : TEXT("No"),
				Var.bReplicated ? TEXT("Yes") : TEXT("No"));
		}
	}

	// --- Property Overrides ---
	if (Data.PropertyOverrides.Num() > 0)
	{
		Result += TEXT("\n## Property Overrides\n");
		for (const FPropertyOverrideData& Override : Data.PropertyOverrides)
		{
			Result += FString::Printf(TEXT("- %s = %s\n"), *Override.Name, *Override.Value);
		}
	}

	// --- Interfaces ---
	if (Data.Interfaces.Num() > 0)
	{
		Result += TEXT("\n## Interfaces\n");
		for (const FString& Iface : Data.Interfaces)
		{
			Result += FString::Printf(TEXT("- %s\n"), *Iface);
		}
	}

	// --- Components ---
	if (Data.Components.Num() > 0)
	{
		Result += TEXT("\n## Components\n");
		Result += TEXT("| Name | Class |\n");
		Result += TEXT("|------|-------|\n");
		for (const FComponentAuditData& Comp : Data.Components)
		{
			Result += FString::Printf(TEXT("| %s | %s |\n"), *Comp.Name, *Comp.Class);
		}
	}

	// --- Timelines ---
	if (Data.Timelines.Num() > 0)
	{
		Result += TEXT("\n## Timelines\n");
		Result += TEXT("| Name | Length | Loop | AutoPlay | Float | Vector | Color | Event |\n");
		Result += TEXT("|------|--------|------|----------|-------|--------|-------|-------|\n");
		for (const FTimelineAuditData& TL : Data.Timelines)
		{
			Result += FString::Printf(TEXT("| %s | %.2f | %s | %s | %d | %d | %d | %d |\n"),
				*TL.Name, TL.Length,
				TL.bLooping ? TEXT("Yes") : TEXT("No"),
				TL.bAutoPlay ? TEXT("Yes") : TEXT("No"),
				TL.FloatTrackCount, TL.VectorTrackCount,
				TL.LinearColorTrackCount, TL.EventTrackCount);
		}
	}

	// --- Widget Tree ---
	if (Data.WidgetTree.IsSet())
	{
		Result += TEXT("\n## Widget Tree\n");
		Result += SerializeWidgetToMarkdown(Data.WidgetTree.GetValue(), 0);
	}

	// --- Event Graphs ---
	for (const FGraphAuditData& Graph : Data.EventGraphs)
	{
		Result += TEXT("\n");
		Result += SerializeGraphToMarkdown(Graph, TEXT("EventGraph"));
	}

	// --- Function Graphs ---
	for (const FGraphAuditData& Graph : Data.FunctionGraphs)
	{
		Result += TEXT("\n");
		Result += SerializeGraphToMarkdown(Graph, TEXT("Function"));
	}

	// --- Macro Graphs ---
	for (const FGraphAuditData& Graph : Data.MacroGraphs)
	{
		Result += TEXT("\n");
		Result += SerializeGraphToMarkdown(Graph, TEXT("Macro"));
	}

	return Result;
}

FString FBlueprintAuditor::SerializeGraphToMarkdown(const FGraphAuditData& Data, const FString& Prefix)
{
	FString Result;
	Result.Reserve(2048);

	// --- Heading ---
	if (Prefix == TEXT("Function"))
	{
		// ## Function: Name(params) -> returns
		Result += TEXT("## Function: ");
		Result += Data.Name;
		Result += TEXT("(");
		for (int32 i = 0; i < Data.Inputs.Num(); ++i)
		{
			if (i > 0) Result += TEXT(", ");
			Result += Data.Inputs[i].Name;
			Result += TEXT(": ");
			Result += Data.Inputs[i].Type;
		}
		Result += TEXT(")");
		if (Data.Outputs.Num() > 0)
		{
			Result += TEXT(" -> ");
			for (int32 i = 0; i < Data.Outputs.Num(); ++i)
			{
				if (i > 0) Result += TEXT(", ");
				Result += Data.Outputs[i].Name;
				Result += TEXT(": ");
				Result += Data.Outputs[i].Type;
			}
		}
		Result += TEXT("\n");
	}
	else if (Prefix == TEXT("Macro"))
	{
		Result += TEXT("## Macro: ");
		Result += Data.Name;
		if (Data.Inputs.Num() > 0 || Data.Outputs.Num() > 0)
		{
			Result += TEXT("(");
			for (int32 i = 0; i < Data.Inputs.Num(); ++i)
			{
				if (i > 0) Result += TEXT(", ");
				Result += Data.Inputs[i].Name;
				Result += TEXT(": ");
				Result += Data.Inputs[i].Type;
			}
			Result += TEXT(")");
			if (Data.Outputs.Num() > 0)
			{
				Result += TEXT(" -> ");
				for (int32 i = 0; i < Data.Outputs.Num(); ++i)
				{
					if (i > 0) Result += TEXT(", ");
					Result += Data.Outputs[i].Name;
					Result += TEXT(": ");
					Result += Data.Outputs[i].Type;
				}
			}
		}
		Result += TEXT("\n");
	}
	else
	{
		Result += FString::Printf(TEXT("## %s\n"), *Data.Name);
	}

	// --- Node table ---
	if (Data.Nodes.Num() > 0)
	{
		Result += TEXT("| Id | Type | Name | Details |\n");
		Result += TEXT("|----|------|------|---------|\n");
		for (const FNodeAuditData& Node : Data.Nodes)
		{
			// Build Details column: target, flags, defaults
			FString Details;
			if (!Node.Target.IsEmpty())
			{
				Details += Node.Target;
			}
			if (Node.bPure)
			{
				if (!Details.IsEmpty()) Details += TEXT(", ");
				Details += TEXT("pure");
			}
			if (Node.bLatent)
			{
				if (!Details.IsEmpty()) Details += TEXT(", ");
				Details += TEXT("latent");
			}
			if (!Node.bIsNative && Node.Type == TEXT("CallFunction"))
			{
				if (!Details.IsEmpty()) Details += TEXT(", ");
				Details += TEXT("not-native");
			}
			if (Node.DefaultInputs.Num() > 0)
			{
				for (const FDefaultInputData& Input : Node.DefaultInputs)
				{
					if (!Details.IsEmpty()) Details += TEXT(", ");
					Details += FString::Printf(TEXT("%s=%s"), *Input.Name, *Input.Value);
				}
			}
			Result += FString::Printf(TEXT("| %d | %s | %s | %s |\n"),
				Node.Id, *Node.Type, *Node.Name, *Details);
		}
	}

	// --- Exec edges (compact one-liners) ---
	if (Data.ExecFlows.Num() > 0)
	{
		Result += TEXT("\nExec: ");
		for (int32 i = 0; i < Data.ExecFlows.Num(); ++i)
		{
			if (i > 0) Result += TEXT(", ");
			const FExecEdge& Edge = Data.ExecFlows[i];
			// Omit pin name when it's "then" (case-insensitive)
			if (Edge.SourcePinName.Equals(TEXT("then"), ESearchCase::IgnoreCase))
			{
				Result += FString::Printf(TEXT("%d->%d"), Edge.SourceNodeId, Edge.TargetNodeId);
			}
			else
			{
				Result += FString::Printf(TEXT("%d-[%s]->%d"),
					Edge.SourceNodeId, *Edge.SourcePinName, Edge.TargetNodeId);
			}
		}
		Result += TEXT("\n");
	}

	// --- Data edges (compact one-liners) ---
	if (Data.DataFlows.Num() > 0)
	{
		Result += TEXT("Data: ");
		for (int32 i = 0; i < Data.DataFlows.Num(); ++i)
		{
			if (i > 0) Result += TEXT(", ");
			const FDataEdge& Edge = Data.DataFlows[i];
			Result += FString::Printf(TEXT("%d.%s->%d.%s"),
				Edge.SourceNodeId, *Edge.SourcePinName,
				Edge.TargetNodeId, *Edge.TargetPinName);
		}
		Result += TEXT("\n");
	}

	return Result;
}

FString FBlueprintAuditor::SerializeWidgetToMarkdown(const FWidgetAuditData& Data, int32 Indent)
{
	FString Result;

	// Build indent string (two spaces per level)
	FString IndentStr;
	for (int32 i = 0; i < Indent; ++i)
	{
		IndentStr += TEXT("  ");
	}

	Result += IndentStr;
	Result += FString::Printf(TEXT("- %s (%s)"), *Data.Name, *Data.Class);
	if (Data.bIsVariable)
	{
		Result += TEXT(" [var]");
	}
	if (!Data.SlotName.IsEmpty())
	{
		Result += FString::Printf(TEXT(" [slot:%s]"), *Data.SlotName);
	}
	Result += TEXT("\n");

	for (const FWidgetAuditData& Child : Data.Children)
	{
		Result += SerializeWidgetToMarkdown(Child, Indent + 1);
	}

	return Result;
}

// ============================================================================
// Legacy synchronous API (thin wrappers over Gather + Serialize)
// ============================================================================

FString FBlueprintAuditor::AuditBlueprint(const UBlueprint* BP)
{
	return SerializeToMarkdown(GatherBlueprintData(BP));
}

FString FBlueprintAuditor::AuditGraph(const UEdGraph* Graph)
{
	return SerializeGraphToMarkdown(GatherGraphData(Graph), TEXT("EventGraph"));
}

FString FBlueprintAuditor::AuditWidget(UWidget* Widget)
{
	return SerializeWidgetToMarkdown(GatherWidgetData(Widget));
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

	return GetAuditBaseDir() / RelativePath + TEXT(".md");
}

bool FBlueprintAuditor::DeleteAuditFile(const FString& FilePath)
{
	IFileManager& FM = IFileManager::Get();
	if (!FM.FileExists(*FilePath))
	{
		return true;
	}

	if (FM.Delete(*FilePath))
	{
		UE_LOG(LogCoRider, Display, TEXT("CoRider: Deleted audit file %s"), *FilePath);
		return true;
	}

	UE_LOG(LogCoRider, Warning, TEXT("CoRider: Failed to delete audit file %s"), *FilePath);
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

bool FBlueprintAuditor::WriteAuditFile(const FString& Content, const FString& OutputPath)
{
	if (FFileHelper::SaveStringToFile(Content, *OutputPath))
	{
		UE_LOG(LogCoRider, Verbose, TEXT("CoRider: Audit saved to %s"), *OutputPath);
		return true;
	}

	UE_LOG(LogCoRider, Error, TEXT("CoRider: Failed to write %s"), *OutputPath);
	return false;
}

bool FBlueprintAuditor::IsSupportedBlueprintClass(const FTopLevelAssetPath& ClassPath)
{
	const FString ClassName = ClassPath.GetAssetName().ToString();

	// ControlRigBlueprint and RigVMBlueprint trigger fatal assertions in
	// RigVMController during LoadObject (InOuter assertion failure) because
	// their subsystems expect a loading context that bare LoadObject cannot provide.
	if (ClassName.Contains(TEXT("ControlRig")) || ClassName.Contains(TEXT("RigVM")))
	{
		return false;
	}

	return true;
}

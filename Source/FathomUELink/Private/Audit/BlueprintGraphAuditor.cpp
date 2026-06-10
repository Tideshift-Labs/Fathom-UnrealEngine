#include "Audit/BlueprintGraphAuditor.h"

#include "Audit/AuditHelpers.h"
#include "Audit/AuditFileUtils.h"
#include "FathomUELinkModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Knot.h"
#include "K2Node_Timeline.h"
#include "K2Node_Tunnel.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlotInterface.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"

// ============================================================================
// Internal helpers (Blueprint-graph-specific)
// ============================================================================

namespace
{
	/**
	 * Follow pin connections through UK2Node_Knot reroute nodes to find real
	 * endpoints. Pin is the original walking pin (an output pin when called
	 * from edge enumeration); we walk downstream through knot chains.
	 *
	 * To continue downstream past a knot we need the knot's same-direction
	 * pin (matching Pin->Direction): for an Output Pin, the knot's Output
	 * pin, whose own LinkedTo gives the next downstream input pins. Picking
	 * the opposite direction would walk back upstream and produce self-edges.
	 */
	TArray<UEdGraphPin*> TraceThroughKnots(UEdGraphPin* Pin, TSet<UEdGraphNode*>& Visited)
	{
		TArray<UEdGraphPin*> Result;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			// Corrupted/partially-loaded graphs can hold null or orphaned
			// pin links. GetOwningNode() would check()-fail on a null owner.
			if (!Linked)
			{
				continue;
			}
			UEdGraphNode* Owner = Linked->GetOwningNodeUnchecked();
			if (!Owner)
			{
				continue;
			}
			if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Owner))
			{
				// Guard against cyclic knot chains (corrupted graphs)
				bool bAlreadyVisited = false;
				Visited.Add(Knot, &bAlreadyVisited);
				if (bAlreadyVisited)
				{
					continue;
				}

				// Recurse from the knot's same-direction pin to continue downstream.
				for (UEdGraphPin* KnotPin : Knot->Pins)
				{
					if (!KnotPin || KnotPin->Direction != Pin->Direction)
					{
						continue;
					}
					Result.Append(TraceThroughKnots(KnotPin, Visited));
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
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
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
// Game-thread gather functions
// ============================================================================

FBlueprintAuditData FBlueprintGraphAuditor::GatherBlueprintData(const UBlueprint* BP)
{
	FBlueprintAuditData Data;

	// --- Metadata ---
	Data.Name = BP->GetName();
	Data.Path = BP->GetPathName();
	Data.PackageName = BP->GetOutermost()->GetName();
	Data.ParentClass = BP->ParentClass ? BP->ParentClass->GetPathName() : TEXT("None");
	Data.BlueprintType = StaticEnum<EBlueprintType>()->GetNameStringByValue(static_cast<int64>(BP->BlueprintType));
	Data.CompileStatus = StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(BP->Status));

	// Store the source file path so the hash can be computed on a background thread
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Gathering data for %s (Parent: %s)"),
		*Data.Name, BP->ParentClass ? *BP->ParentClass->GetName() : TEXT("None"));

	// --- Variables ---
	Data.Variables.Reserve(BP->NewVariables.Num());
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		FVariableAuditData VarData;
		VarData.Name = Var.VarName.ToString();
		VarData.Type = FAuditFileUtils::GetVariableTypeString(Var.VarType);
		VarData.Category = Var.Category.ToString();
		VarData.bInstanceEditable =
			Var.HasMetaData(FBlueprintMetadata::MD_Private) == false &&
			(Var.PropertyFlags & CPF_Edit) != 0;
		VarData.bReplicated = (Var.PropertyFlags & CPF_Net) != 0;
		Data.Variables.Add(MoveTemp(VarData));
	}

	// --- Property Overrides (CDO Diff) ---
	// Skip if the Blueprint has compile errors; the generated class may exist
	// as a skeleton with no usable CDO, which would crash GetDefaultObject().
	if (UClass* GeneratedClass = BP->GeneratedClass)
	{
		if (BP->Status != BS_Error && GeneratedClass->GetSuperClass())
		{
			UClass* SuperClass = GeneratedClass->GetSuperClass();
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
					Override.Value = FathomAuditHelpers::FormatPropertyValue(Prop, ValuePtr, /*IndentDepth=*/0);
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
		// Build a map from node to parent for hierarchy
		TMap<const USCS_Node*, const USCS_Node*> ParentMap;
		for (const USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node) continue;
			for (const USCS_Node* Child : Node->ChildNodes)
			{
				if (Child)
				{
					ParentMap.Add(Child, Node);
				}
			}
		}

		for (const USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node || !Node->ComponentClass) continue;

			FComponentAuditData CompData;
			CompData.Name = Node->GetVariableName().ToString();
			CompData.Class = Node->ComponentClass->GetName();

			// Parent hierarchy
			if (const USCS_Node** ParentPtr = ParentMap.Find(Node))
			{
				CompData.ParentComponentName = (*ParentPtr)->GetVariableName().ToString();
			}

			// Per-component property overrides (compare template against class defaults)
			if (UActorComponent* Template = Node->ComponentTemplate)
			{
				const UObject* ClassDefaults = Node->ComponentClass->GetDefaultObject();
				for (TFieldIterator<FProperty> PropIt(Node->ComponentClass); PropIt; ++PropIt)
				{
					const FProperty* Prop = *PropIt;

					if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_Config | CPF_DisableEditOnInstance))
					{
						continue;
					}

					if (Prop->HasAnyPropertyFlags(CPF_Transient))
					{
						continue;
					}

					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
					const void* DefaultPtr = Prop->ContainerPtrToValuePtr<void>(ClassDefaults);

					if (!Prop->Identical(ValuePtr, DefaultPtr))
					{
						FPropertyOverrideData Override;
						Override.Name = Prop->GetName();
						Override.Value = FathomAuditHelpers::FormatPropertyValue(Prop, ValuePtr, /*IndentDepth=*/0);
						CompData.PropertyOverrides.Add(MoveTemp(Override));
					}
				}
			}

			Data.Components.Add(MoveTemp(CompData));
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
		if (!Graph) continue;
		Data.EventGraphs.Add(GatherGraphData(Graph));
	}

	// --- Function Graphs ---
	Data.FunctionGraphs.Reserve(BP->FunctionGraphs.Num());
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph) continue;
		Data.FunctionGraphs.Add(GatherGraphData(Graph));
	}

	// --- Macro Graphs (full topology, same as event/function graphs) ---
	Data.MacroGraphs.Reserve(BP->MacroGraphs.Num());
	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		if (!Graph) continue;
		Data.MacroGraphs.Add(GatherGraphData(Graph));
	}

	return Data;
}

FGraphAuditData FBlueprintGraphAuditor::GatherGraphData(const UEdGraph* Graph)
{
	FGraphAuditData Data;
	if (!Graph)
	{
		return Data;
	}
	Data.Name = Graph->GetName();

	// ---- Pass 1: Build node list ----

	TMap<UEdGraphNode*, int32> NodeIdMap;
	int32 NextId = 0;
	TArray<FGraphAuditData> CollectedSubGraphs;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		// Skip null entries (corrupted graphs) and reroute/knot nodes
		if (!Node || Cast<UK2Node_Knot>(Node))
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
				if (!Pin) continue;
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->bHidden) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

				FGraphParamData Param;
				Param.Name = Pin->PinName.ToString();
				Param.Type = FAuditFileUtils::GetVariableTypeString(Pin->PinType);
				Data.Inputs.Add(MoveTemp(Param));
			}
		}
		else if (Cast<UK2Node_FunctionResult>(Node))
		{
			NodeData.Type = TEXT("FunctionResult");
			NodeData.Name = TEXT("Return");

			// All FunctionResult nodes in a graph share the same signature, so only
			// collect outputs from the first one to avoid duplicating return params.
			if (Data.Outputs.Num() == 0)
			{
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin) continue;
					if (Pin->Direction != EGPD_Input) continue;
					if (Pin->bHidden) continue;
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

					FGraphParamData Param;
					Param.Name = Pin->PinName.ToString();
					Param.Type = FAuditFileUtils::GetVariableTypeString(Pin->PinType);
					Data.Outputs.Add(MoveTemp(Param));
				}
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
		// Check Composite before Tunnel (UK2Node_Composite inherits from UK2Node_Tunnel)
		else if (const UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node))
		{
			NodeData.Type = TEXT("CollapsedNode");
			NodeData.Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

			if (UEdGraph* BoundGraph = CompositeNode->BoundGraph)
			{
				CollectedSubGraphs.Add(GatherGraphData(BoundGraph));
			}
		}
		else if (Cast<UK2Node_Tunnel>(Node))
		{
			NodeData.Type = TEXT("Tunnel");
			NodeData.Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}
		else
		{
			NodeData.Type = TEXT("Other");
			NodeData.Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}

		// Capture default values for unconnected input pins (all node types)
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->bHidden) continue;
			if (Pin->LinkedTo.Num() > 0) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;

			FString DisplayValue;
			if (!Pin->DefaultValue.IsEmpty())
			{
				DisplayValue = Pin->DefaultValue;
			}
			else if (Pin->DefaultObject != nullptr)
			{
				DisplayValue = Pin->DefaultObject->GetName();
			}
			else if (!Pin->DefaultTextValue.IsEmpty())
			{
				DisplayValue = Pin->DefaultTextValue.ToString();
			}

			if (!DisplayValue.IsEmpty())
			{
				FDefaultInputData InputData;
				InputData.Name = Pin->PinName.ToString();
				InputData.Value = MoveTemp(DisplayValue);
				NodeData.DefaultInputs.Add(MoveTemp(InputData));
			}
		}

		// Capture compiler message if the node has one
		if (Node->bHasCompilerMessage && !Node->ErrorMsg.IsEmpty())
		{
			FString Severity;
			// ErrorType is ordered most-to-least severe. Use <= so that
			// CriticalError (removed in 5.7, value 0 in 5.5/5.6) is
			// covered without referencing the deprecated enumerator.
			if (Node->ErrorType <= EMessageSeverity::Error)
			{
				Severity = TEXT("Error");
			}
			else if (Node->ErrorType <= EMessageSeverity::Warning)
			{
				Severity = TEXT("Warning");
			}
			else
			{
				Severity = TEXT("Info");
			}
			NodeData.CompilerMessage = FString::Printf(TEXT("%s: %s"), *Severity, *Node->ErrorMsg);
		}

		Data.Nodes.Add(MoveTemp(NodeData));
	}

	// Move collected collapsed sub-graphs into Data
	Data.SubGraphs = MoveTemp(CollectedSubGraphs);

	// ---- Pass 2: Build edges (walk OUTPUT pins only to avoid duplicates) ----

	for (const auto& Pair : NodeIdMap)
	{
		UEdGraphNode* Node = Pair.Key;
		const int32 SourceId = Pair.Value;

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->Direction != EGPD_Output) continue;
			if (Pin->bHidden) continue;
			if (Pin->LinkedTo.Num() == 0) continue;

			const bool bIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			const FString SrcPinName = Pin->PinName.ToString();

			// Resolve through knot/reroute nodes (visited set prevents cycles)
			TSet<UEdGraphNode*> Visited;
			TArray<UEdGraphPin*> ResolvedPins = TraceThroughKnots(const_cast<UEdGraphPin*>(Pin), Visited);

			for (UEdGraphPin* TargetPin : ResolvedPins)
			{
				UEdGraphNode* TargetNode = TargetPin->GetOwningNodeUnchecked();
				if (!TargetNode) continue;
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

FWidgetAuditData FBlueprintGraphAuditor::GatherWidgetData(UWidget* Widget)
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

FString FBlueprintGraphAuditor::SerializeToMarkdown(const FBlueprintAuditData& Data)
{
	FString Result;
	Result.Reserve(4096);

	// --- Header block ---
	Result += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Result += FString::Printf(TEXT("Path: %s\n"), *Data.Path);
	Result += FString::Printf(TEXT("Parent: %s\n"), *Data.ParentClass);
	Result += FString::Printf(TEXT("Type: %s\n"), *Data.BlueprintType);
	if (!Data.CompileStatus.IsEmpty())
	{
		Result += FString::Printf(TEXT("Status: %s\n"), *Data.CompileStatus);
	}

	if (!Data.SourceFilePath.IsEmpty())
	{
		Result += FString::Printf(TEXT("SourcePath: %s\n"), *FAuditFileUtils::ToProjectRelativeSourcePath(Data.SourceFilePath));
		Result += FString::Printf(TEXT("Hash: %s\n"), *FAuditFileUtils::ComputeFileHash(Data.SourceFilePath));
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
		FathomAuditHelpers::FPropertyRenderStyle Style;
		Style.Indent = TEXT("");
		Style.bUseBullet = true;
		Style.InlineSeparator = TEXT(" = ");
		FathomAuditHelpers::SerializePropertyOverridesToMarkdown(Result, Data.PropertyOverrides, Style);
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

		// Build hierarchy tree using parent references
		TMap<FString, TArray<const FComponentAuditData*>> ChildMap;
		TArray<const FComponentAuditData*> Roots;

		for (const FComponentAuditData& Comp : Data.Components)
		{
			if (Comp.ParentComponentName.IsEmpty())
			{
				Roots.Add(&Comp);
			}
			else
			{
				ChildMap.FindOrAdd(Comp.ParentComponentName).Add(&Comp);
			}
		}

		// Recursive tree renderer
		TFunction<void(const FComponentAuditData*, int32)> RenderTree =
			[&](const FComponentAuditData* Comp, int32 Depth)
		{
			for (int32 i = 0; i < Depth; ++i) Result += TEXT("  ");
			Result += FString::Printf(TEXT("- %s (%s)\n"), *Comp->Name, *Comp->Class);

			if (const TArray<const FComponentAuditData*>* Children = ChildMap.Find(Comp->Name))
			{
				for (const FComponentAuditData* Child : *Children)
				{
					RenderTree(Child, Depth + 1);
				}
			}
		};

		for (const FComponentAuditData* Root : Roots)
		{
			RenderTree(Root, 0);
		}

		// Per-component property override sections
		for (const FComponentAuditData& Comp : Data.Components)
		{
			if (Comp.PropertyOverrides.Num() == 0) continue;

			Result += FString::Printf(TEXT("\n### %s (%s)\n"), *Comp.Name, *Comp.Class);
			FathomAuditHelpers::FPropertyRenderStyle CompStyle;
			CompStyle.Indent = TEXT("");
			CompStyle.bUseBullet = true;
			CompStyle.InlineSeparator = TEXT(" = ");
			FathomAuditHelpers::SerializePropertyOverridesToMarkdown(Result, Comp.PropertyOverrides, CompStyle);
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

// ============================================================================
// Graph partitioning helpers (issue #38: separate event-graph entry-point flows)
// ============================================================================

namespace
{
	/**
	 * Build a "## " / "### " heading prefix at the given level. Caps at h6.
	 */
	FString MakeHeadingHashes(int32 Level)
	{
		const int32 N = FMath::Clamp(Level, 1, 6);
		FString Out;
		Out.Reserve(N + 1);
		for (int32 i = 0; i < N; ++i) Out += TEXT("#");
		Out += TEXT(" ");
		return Out;
	}

	/**
	 * A node is an exec entry iff it has at least one outgoing exec edge AND zero
	 * incoming exec edges. This catches Event / CustomEvent / FunctionEntry /
	 * Tunnel(Inputs) / input-action Other nodes uniformly via topology.
	 */
	TArray<int32> FindEntryNodeIds(const FGraphAuditData& Data)
	{
		TSet<int32> WithIncomingExec;
		TSet<int32> WithOutgoingExec;
		for (const FExecEdge& E : Data.ExecFlows)
		{
			WithIncomingExec.Add(E.TargetNodeId);
			WithOutgoingExec.Add(E.SourceNodeId);
		}
		TArray<int32> Entries;
		for (const FNodeAuditData& N : Data.Nodes)
		{
			if (WithOutgoingExec.Contains(N.Id) && !WithIncomingExec.Contains(N.Id))
			{
				Entries.Add(N.Id);
			}
		}
		Entries.Sort();
		return Entries;
	}

	struct FGraphPartition
	{
		TArray<int32> EntryIds;
		TMap<int32, TSet<int32>> NodesByEntry;       // entry id -> nodes in section
		TMap<int32, TArray<int32>> EntriesPerNode;   // node id -> sorted list of entries that own it
		TArray<int32> OrphanNodeIds;                 // not reachable from any entry
	};

	/**
	 * For each entry: forward-BFS along exec to collect reachable exec nodes, then
	 * close over data-input dependencies (recursing through pure sources). Records
	 * both per-entry membership and the inverted entries-per-node map for shared
	 * annotations.
	 */
	FGraphPartition ComputePartition(const FGraphAuditData& Data, const TArray<int32>& EntryIds)
	{
		FGraphPartition Part;
		Part.EntryIds = EntryIds;

		TMap<int32, TArray<int32>> OutExec;
		TMap<int32, TArray<int32>> InData;
		for (const FExecEdge& E : Data.ExecFlows)
		{
			OutExec.FindOrAdd(E.SourceNodeId).Add(E.TargetNodeId);
		}
		for (const FDataEdge& D : Data.DataFlows)
		{
			InData.FindOrAdd(D.TargetNodeId).Add(D.SourceNodeId);
		}

		// "Pure" for the closure-recursion decision: marked pure OR not part of any exec edge.
		TSet<int32> AnyExec;
		for (const FExecEdge& E : Data.ExecFlows)
		{
			AnyExec.Add(E.SourceNodeId);
			AnyExec.Add(E.TargetNodeId);
		}
		TMap<int32, bool> IsPure;
		for (const FNodeAuditData& N : Data.Nodes)
		{
			IsPure.Add(N.Id, N.bPure || !AnyExec.Contains(N.Id));
		}

		for (int32 Entry : EntryIds)
		{
			TSet<int32> Section;
			TArray<int32> Queue;
			Section.Add(Entry);
			Queue.Add(Entry);

			// Forward exec BFS.
			while (Queue.Num() > 0)
			{
				const int32 N = Queue.Pop(EAllowShrinking::No);
				if (const TArray<int32>* Outs = OutExec.Find(N))
				{
					for (int32 T : *Outs)
					{
						bool bAlready = false;
						Section.Add(T, &bAlready);
						if (!bAlready) Queue.Add(T);
					}
				}
			}

			// Data-input closure: for each node currently in Section, pull its data
			// sources. Recurse through pure sources only (impure nodes anchor on their
			// own exec chain elsewhere).
			TArray<int32> DataQueue = Section.Array();
			while (DataQueue.Num() > 0)
			{
				const int32 N = DataQueue.Pop(EAllowShrinking::No);
				if (const TArray<int32>* Sources = InData.Find(N))
				{
					for (int32 S : *Sources)
					{
						bool bAlready = false;
						Section.Add(S, &bAlready);
						if (!bAlready && IsPure.FindRef(S))
						{
							DataQueue.Add(S);
						}
					}
				}
			}

			for (int32 N : Section)
			{
				Part.EntriesPerNode.FindOrAdd(N).Add(Entry);
			}
			Part.NodesByEntry.Add(Entry, MoveTemp(Section));
		}

		// Sort entries-per-node lists for deterministic shared-annotation output.
		for (auto& Kvp : Part.EntriesPerNode)
		{
			Kvp.Value.Sort();
		}

		// Orphans = any node not in any entry's section (typically standalone comments
		// or disconnected dead code).
		for (const FNodeAuditData& N : Data.Nodes)
		{
			if (!Part.EntriesPerNode.Contains(N.Id))
			{
				Part.OrphanNodeIds.Add(N.Id);
			}
		}
		return Part;
	}

	/** Display name for an entry node, used in shared-annotation lists. */
	FString MakeEntryDisplayName(const FNodeAuditData& Node)
	{
		return Node.Name.IsEmpty() ? Node.Type : Node.Name;
	}

	/** Render one node-table row. SharedWith is empty for the flat / single-entry case. */
	void AppendNodeRow(FString& Out, const FNodeAuditData& Node, const TArray<FString>& SharedWith)
	{
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
		for (const FDefaultInputData& Input : Node.DefaultInputs)
		{
			if (!Details.IsEmpty()) Details += TEXT(", ");
			Details += FString::Printf(TEXT("%s=%s"), *Input.Name, *Input.Value);
		}
		if (!Node.CompilerMessage.IsEmpty())
		{
			if (!Details.IsEmpty()) Details += TEXT(", ");
			Details += Node.CompilerMessage;
		}
		if (SharedWith.Num() > 0)
		{
			if (!Details.IsEmpty()) Details += TEXT(", ");
			Details += TEXT("[shared with: ");
			for (int32 i = 0; i < SharedWith.Num(); ++i)
			{
				if (i > 0) Details += TEXT(", ");
				Details += SharedWith[i];
			}
			Details += TEXT("]");
		}
		Out += FString::Printf(TEXT("| %d | %s | %s | %s |\n"),
			Node.Id, *Node.Type, *Node.Name, *Details);
	}

	/**
	 * Render a node table for a subset of node ids (preserving Data.Nodes order, which
	 * is id-ordered). When EntriesPerNode is non-null and a node is shared across
	 * multiple entries, the row gets a "[shared with: ...]" annotation excluding the
	 * current section's entry id.
	 */
	void AppendNodeTable(FString& Out,
	                     const FGraphAuditData& Data,
	                     const TSet<int32>& NodeIds,
	                     const TMap<int32, FString>& EntryDisplayNameById,
	                     const TMap<int32, TArray<int32>>* EntriesPerNode,
	                     int32 OmitFromSharedListId)
	{
		if (NodeIds.Num() == 0) return;
		Out += TEXT("| Id | Type | Name | Details |\n");
		Out += TEXT("|----|------|------|---------|\n");
		for (const FNodeAuditData& Node : Data.Nodes)
		{
			if (!NodeIds.Contains(Node.Id)) continue;
			TArray<FString> SharedWith;
			if (EntriesPerNode)
			{
				if (const TArray<int32>* Entries = EntriesPerNode->Find(Node.Id))
				{
					if (Entries->Num() > 1)
					{
						for (int32 EId : *Entries)
						{
							if (EId == OmitFromSharedListId) continue;
							SharedWith.Add(EntryDisplayNameById.FindRef(EId));
						}
					}
				}
			}
			AppendNodeRow(Out, Node, SharedWith);
		}
	}

	/**
	 * Append "Exec: ..." and "Data: ..." lines limited to edges where both endpoints
	 * are in NodeIds. Pin-name elision for "then" mirrors the flat renderer.
	 */
	void AppendExecAndDataLines(FString& Out, const FGraphAuditData& Data, const TSet<int32>& NodeIds)
	{
		bool bWroteExec = false;
		for (const FExecEdge& E : Data.ExecFlows)
		{
			if (!NodeIds.Contains(E.SourceNodeId) || !NodeIds.Contains(E.TargetNodeId)) continue;
			if (!bWroteExec)
			{
				Out += TEXT("\nExec: ");
				bWroteExec = true;
			}
			else
			{
				Out += TEXT(", ");
			}
			if (E.SourcePinName.Equals(TEXT("then"), ESearchCase::IgnoreCase))
			{
				Out += FString::Printf(TEXT("%d->%d"), E.SourceNodeId, E.TargetNodeId);
			}
			else
			{
				Out += FString::Printf(TEXT("%d-[%s]->%d"), E.SourceNodeId, *E.SourcePinName, E.TargetNodeId);
			}
		}
		if (bWroteExec) Out += TEXT("\n");

		bool bWroteData = false;
		for (const FDataEdge& D : Data.DataFlows)
		{
			if (!NodeIds.Contains(D.SourceNodeId) || !NodeIds.Contains(D.TargetNodeId)) continue;
			if (!bWroteData)
			{
				Out += TEXT("Data: ");
				bWroteData = true;
			}
			else
			{
				Out += TEXT(", ");
			}
			Out += FString::Printf(TEXT("%d.%s->%d.%s"),
				D.SourceNodeId, *D.SourcePinName, D.TargetNodeId, *D.TargetPinName);
		}
		if (bWroteData) Out += TEXT("\n");
	}

	/** Forward declaration so internal recursion can target the level-aware variant. */
	FString SerializeGraphAtLevel(const FGraphAuditData& Data, const FString& Prefix, int32 HeadingLevel);

	void AppendGraphHeading(FString& Out, const FGraphAuditData& Data, const FString& Prefix, int32 HeadingLevel)
	{
		const FString H = MakeHeadingHashes(HeadingLevel);

		if (Prefix == TEXT("Function"))
		{
			Out += H + TEXT("Function: ") + Data.Name + TEXT("(");
			for (int32 i = 0; i < Data.Inputs.Num(); ++i)
			{
				if (i > 0) Out += TEXT(", ");
				Out += Data.Inputs[i].Name + TEXT(": ") + Data.Inputs[i].Type;
			}
			Out += TEXT(")");
			if (Data.Outputs.Num() > 0)
			{
				Out += TEXT(" -> ");
				for (int32 i = 0; i < Data.Outputs.Num(); ++i)
				{
					if (i > 0) Out += TEXT(", ");
					Out += Data.Outputs[i].Name + TEXT(": ") + Data.Outputs[i].Type;
				}
			}
			Out += TEXT("\n");
		}
		else if (Prefix == TEXT("Macro"))
		{
			Out += H + TEXT("Macro: ") + Data.Name;
			if (Data.Inputs.Num() > 0 || Data.Outputs.Num() > 0)
			{
				Out += TEXT("(");
				for (int32 i = 0; i < Data.Inputs.Num(); ++i)
				{
					if (i > 0) Out += TEXT(", ");
					Out += Data.Inputs[i].Name + TEXT(": ") + Data.Inputs[i].Type;
				}
				Out += TEXT(")");
				if (Data.Outputs.Num() > 0)
				{
					Out += TEXT(" -> ");
					for (int32 i = 0; i < Data.Outputs.Num(); ++i)
					{
						if (i > 0) Out += TEXT(", ");
						Out += Data.Outputs[i].Name + TEXT(": ") + Data.Outputs[i].Type;
					}
				}
			}
			Out += TEXT("\n");
		}
		else if (Prefix == TEXT("Collapsed"))
		{
			Out += FString::Printf(TEXT("%sCollapsed: %s\n"), *H, *Data.Name);
		}
		else
		{
			Out += FString::Printf(TEXT("%s%s\n"), *H, *Data.Name);
		}
	}

	FString SerializeGraphAtLevel(const FGraphAuditData& Data, const FString& Prefix, int32 HeadingLevel)
	{
		FString Result;
		Result.Reserve(2048);

		AppendGraphHeading(Result, Data, Prefix, HeadingLevel);

		// Decide whether to partition by entry node. Functions/macros and simple
		// single-event graphs fall back to the original flat layout.
		const TArray<int32> EntryIds = FindEntryNodeIds(Data);
		const bool bPartition = EntryIds.Num() > 1;

		if (!bPartition)
		{
			TSet<int32> AllIds;
			AllIds.Reserve(Data.Nodes.Num());
			for (const FNodeAuditData& N : Data.Nodes) AllIds.Add(N.Id);
			AppendNodeTable(Result, Data, AllIds, /*EntryNames=*/{}, /*EntriesPerNode=*/nullptr, INDEX_NONE);
			AppendExecAndDataLines(Result, Data, AllIds);
		}
		else
		{
			const FGraphPartition Part = ComputePartition(Data, EntryIds);

			TMap<int32, FString> EntryDisplayNameById;
			for (const FNodeAuditData& N : Data.Nodes)
			{
				if (Part.NodesByEntry.Contains(N.Id))
				{
					EntryDisplayNameById.Add(N.Id, MakeEntryDisplayName(N));
				}
			}

			const FString EntryHeading = MakeHeadingHashes(HeadingLevel + 1);
			for (int32 EId : EntryIds)
			{
				Result += FString::Printf(TEXT("\n%sEntry: %s\n"),
					*EntryHeading, *EntryDisplayNameById[EId]);
				const TSet<int32>& Section = Part.NodesByEntry[EId];
				AppendNodeTable(Result, Data, Section, EntryDisplayNameById, &Part.EntriesPerNode, EId);
				AppendExecAndDataLines(Result, Data, Section);
			}

			if (Part.OrphanNodeIds.Num() > 0)
			{
				Result += FString::Printf(TEXT("\n%sOther Nodes\n"), *EntryHeading);
				const TSet<int32> OrphanSet(Part.OrphanNodeIds);
				AppendNodeTable(Result, Data, OrphanSet, EntryDisplayNameById, /*EntriesPerNode=*/nullptr, INDEX_NONE);
				// Orphans have no edges to other nodes; AppendExecAndDataLines would emit nothing.
			}
		}

		// Recursive sub-graphs nest one heading level deeper.
		for (const FGraphAuditData& Sub : Data.SubGraphs)
		{
			Result += TEXT("\n");
			Result += SerializeGraphAtLevel(Sub, TEXT("Collapsed"), HeadingLevel + 1);
		}

		return Result;
	}
}

FString FBlueprintGraphAuditor::SerializeGraphToMarkdown(const FGraphAuditData& Data, const FString& Prefix)
{
	// Top-level graphs (EventGraph / Function / Macro) render at heading level 2.
	// Collapsed sub-graphs reached via the recursion bump to level 3+.
	return SerializeGraphAtLevel(Data, Prefix, 2);
}

FString FBlueprintGraphAuditor::SerializeWidgetToMarkdown(const FWidgetAuditData& Data, int32 Indent)
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

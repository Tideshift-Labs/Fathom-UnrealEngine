#include "StateTreeAuditor.h"

#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeEditorNode.h"
#include "StateTreeTypes.h"
#include "StateTreeTasksStatus.h"
#include "Audit/AuditFileUtils.h"
#include "FathomUELinkModule.h"
#include "PropertyBindingPath.h"
#include "PropertyBindingBindingCollection.h"
#include "PropertyBindingBindableStructDescriptor.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"

// ---------------------------------------------------------------------------
// Enum-to-string helpers
// ---------------------------------------------------------------------------

static FString StateTypeToString(EStateTreeStateType Type)
{
	switch (Type)
	{
	case EStateTreeStateType::State:       return TEXT("State");
	case EStateTreeStateType::Group:       return TEXT("Group");
	case EStateTreeStateType::Linked:      return TEXT("Linked");
	case EStateTreeStateType::LinkedAsset: return TEXT("LinkedAsset");
	case EStateTreeStateType::Subtree:     return TEXT("Subtree");
	default:                               return TEXT("Unknown");
	}
}

static FString TransitionTriggerToString(EStateTreeTransitionTrigger Trigger)
{
	const uint8 Val = static_cast<uint8>(Trigger);
	if (Val == 0) return TEXT("None");

	TArray<FString> Parts;
	if (Val & static_cast<uint8>(EStateTreeTransitionTrigger::OnStateSucceeded))
		Parts.Add(TEXT("OnStateSucceeded"));
	if (Val & static_cast<uint8>(EStateTreeTransitionTrigger::OnStateFailed))
		Parts.Add(TEXT("OnStateFailed"));
	if (Val & static_cast<uint8>(EStateTreeTransitionTrigger::OnTick))
		Parts.Add(TEXT("OnTick"));
	if (Val & static_cast<uint8>(EStateTreeTransitionTrigger::OnEvent))
		Parts.Add(TEXT("OnEvent"));
	if (Val & static_cast<uint8>(EStateTreeTransitionTrigger::OnDelegate))
		Parts.Add(TEXT("OnDelegate"));

	// OnStateCompleted = OnStateSucceeded | OnStateFailed
	// If both are present, simplify to "OnStateCompleted"
	if (Parts.Contains(TEXT("OnStateSucceeded")) && Parts.Contains(TEXT("OnStateFailed")))
	{
		Parts.Remove(TEXT("OnStateSucceeded"));
		Parts.Remove(TEXT("OnStateFailed"));
		Parts.Insert(TEXT("OnStateCompleted"), 0);
	}

	return FString::Join(Parts, TEXT("|"));
}

static FString TransitionTypeToString(EStateTreeTransitionType Type)
{
	switch (Type)
	{
	case EStateTreeTransitionType::None:              return TEXT("None");
	case EStateTreeTransitionType::Succeeded:         return TEXT("Succeeded");
	case EStateTreeTransitionType::Failed:             return TEXT("Failed");
	case EStateTreeTransitionType::GotoState:          return TEXT("GotoState");
	case EStateTreeTransitionType::NextState:          return TEXT("NextState");
	case EStateTreeTransitionType::NextSelectableState: return TEXT("NextSelectableState");
	default:                                           return TEXT("Unknown");
	}
}

static FString PriorityToString(EStateTreeTransitionPriority Priority)
{
	switch (Priority)
	{
	case EStateTreeTransitionPriority::None:     return TEXT("None");
	case EStateTreeTransitionPriority::Low:      return TEXT("Low");
	case EStateTreeTransitionPriority::Normal:   return TEXT("Normal");
	case EStateTreeTransitionPriority::Medium:   return TEXT("Medium");
	case EStateTreeTransitionPriority::High:     return TEXT("High");
	case EStateTreeTransitionPriority::Critical: return TEXT("Critical");
	default:                                     return TEXT("Unknown");
	}
}

static FString SelectionBehaviorToString(EStateTreeStateSelectionBehavior Behavior)
{
	switch (Behavior)
	{
	case EStateTreeStateSelectionBehavior::None:                                      return TEXT("None");
	case EStateTreeStateSelectionBehavior::TryEnterState:                             return TEXT("Try Enter");
	case EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder:                  return TEXT("Try Select Children In Order");
	case EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom:                 return TEXT("Try Select Children At Random");
	case EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility:       return TEXT("Try Select Children With Highest Utility");
	case EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandomWeightedByUtility: return TEXT("Try Select Children At Random Weighted By Utility");
	case EStateTreeStateSelectionBehavior::TryFollowTransitions:                      return TEXT("Try Follow Transitions");
	default:                                                                          return TEXT("Unknown");
	}
}

static FString ExpressionOperandToString(EStateTreeExpressionOperand Operand)
{
	switch (Operand)
	{
	case EStateTreeExpressionOperand::Copy:     return TEXT("");
	case EStateTreeExpressionOperand::And:      return TEXT("AND");
	case EStateTreeExpressionOperand::Or:       return TEXT("OR");
	case EStateTreeExpressionOperand::Multiply: return TEXT("MUL");
	default:                                    return TEXT("");
	}
}

// ---------------------------------------------------------------------------
// Property gathering (inline, self-contained)
// ---------------------------------------------------------------------------

/** Returns true if the exported value looks like uninitialized/sentinel data (FLT_MAX). */
static bool IsSentinelValue(const FString& Value)
{
	return Value.Contains(TEXT("340282346638528"));
}

/** Resolve enum display name if the value is a raw internal name like "NewEnumerator3". */
static FString ResolveEnumDisplayName(const FProperty* Prop, const FString& RawValue)
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

	if (!Enum)
	{
		return RawValue;
	}

	const int64 EnumValue = Enum->GetValueByNameString(RawValue);
	if (EnumValue == INDEX_NONE)
	{
		return RawValue;
	}

	const FText DisplayName = Enum->GetDisplayNameTextByValue(EnumValue);
	if (!DisplayName.IsEmpty())
	{
		return DisplayName.ToString();
	}

	return RawValue;
}

/** Clean trailing decimal zeros: 0.500000 -> 0.5 */
static FString CleanDecimalValue(const FString& Raw)
{
	if (!Raw.Contains(TEXT(".")))
		return Raw;

	FString Result = Raw;
	while (Result.EndsWith(TEXT("0")))
		Result.LeftChopInline(1);
	if (Result.EndsWith(TEXT(".")))
		Result.LeftChopInline(1);
	return Result;
}

/**
 * Gather editor-visible properties from an FInstancedStruct's UScriptStruct.
 * Returns properties that are EditAnywhere and not transient/deprecated.
 */
static TArray<FPropertyOverrideData> GatherStructProperties(const FInstancedStruct& Instance)
{
	TArray<FPropertyOverrideData> Props;

	const UScriptStruct* ScriptStruct = Instance.GetScriptStruct();
	if (!ScriptStruct)
		return Props;

	const uint8* StructMemory = Instance.GetMemory();
	if (!StructMemory)
		return Props;

	for (TFieldIterator<FProperty> PropIt(ScriptStruct); PropIt; ++PropIt)
	{
		const FProperty* Prop = *PropIt;

		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
			continue;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			continue;

		if (Prop->HasMetaData(TEXT("InlineEditConditionToggle")))
			continue;

		// Skip StateTree base class internals and editor-only visual properties
		static const TSet<FString> SkipNames = {
			TEXT("Name"), TEXT("bEnabled"), TEXT("bTaskEnabled"),
			TEXT("bRunDuringTransition"), TEXT("IconColor"),
			TEXT("bShouldStateChangeOnReselect"),
			TEXT("bShouldCopyBoundPropertiesOnTick"),
			TEXT("bShouldCopyBoundPropertiesOnExitState"),
			TEXT("bConsideredForCompletion"),
			TEXT("bCanEditConsideredForCompletion"),
		};
		if (SkipNames.Contains(Prop->GetName()))
			continue;

		FString Value;
		Prop->ExportText_Direct(Value, StructMemory + Prop->GetOffset_ForInternal(), nullptr, nullptr, PPF_None);
		Value = ResolveEnumDisplayName(Prop, Value);
		Value = CleanDecimalValue(Value);

		if (Value.IsEmpty() || Value == TEXT("()") || Value == TEXT("None") || IsSentinelValue(Value))
			continue;

		FPropertyOverrideData Override;
		Override.Name = Prop->GetAuthoredName();
		Override.Value = MoveTemp(Value);
		Props.Add(MoveTemp(Override));
	}

	return Props;
}

/**
 * Gather editor-visible properties from a UObject (Blueprint-based StateTree nodes).
 * Skips base class internals and non-editable properties.
 */
static TArray<FPropertyOverrideData> GatherObjectProperties(const UObject* Object)
{
	TArray<FPropertyOverrideData> Props;
	if (!Object) return Props;

	for (TFieldIterator<FProperty> PropIt(Object->GetClass()); PropIt; ++PropIt)
	{
		const FProperty* Prop = *PropIt;

		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
			continue;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			continue;

		if (Prop->HasMetaData(TEXT("InlineEditConditionToggle")))
			continue;

		// Skip StateTree base class internals and editor-only visual properties
		static const TSet<FString> SkipNames = {
			TEXT("Name"), TEXT("bEnabled"), TEXT("bTaskEnabled"),
			TEXT("bRunDuringTransition"), TEXT("IconColor"),
			TEXT("bShouldStateChangeOnReselect"),
			TEXT("bShouldCopyBoundPropertiesOnTick"),
			TEXT("bShouldCopyBoundPropertiesOnExitState"),
			TEXT("bConsideredForCompletion"),
			TEXT("bCanEditConsideredForCompletion"),
		};
		if (SkipNames.Contains(Prop->GetName()))
			continue;

		FString Value;
		Prop->ExportText_InContainer(0, Value, Object, nullptr, nullptr, PPF_None);
		Value = ResolveEnumDisplayName(Prop, Value);
		Value = CleanDecimalValue(Value);

		if (Value.IsEmpty() || Value == TEXT("()") || Value == TEXT("None") || IsSentinelValue(Value))
			continue;

		FPropertyOverrideData Override;
		Override.Name = Prop->GetAuthoredName();
		Override.Value = MoveTemp(Value);
		Props.Add(MoveTemp(Override));
	}

	return Props;
}

// ---------------------------------------------------------------------------
// Binding resolution
// ---------------------------------------------------------------------------

/**
 * Build a mapping from struct GUIDs to human-readable names for binding source resolution.
 * Covers Parameters, Evaluators, GlobalTasks, and all state nodes.
 */
static void AddEditorNodesToMap(TMap<FGuid, FString>& Map, const TArray<FStateTreeEditorNode>& Nodes)
{
	for (const FStateTreeEditorNode& Node : Nodes)
	{
		Map.Add(Node.ID, Node.GetName().ToString());
	}
}

static void AddStateNodesToMap(TMap<FGuid, FString>& Map, const UStateTreeState* State)
{
	if (!State) return;

	AddEditorNodesToMap(Map, State->Tasks);
	if (State->SingleTask.Node.IsValid())
	{
		Map.Add(State->SingleTask.ID, State->SingleTask.GetName().ToString());
	}
	AddEditorNodesToMap(Map, State->EnterConditions);
	AddEditorNodesToMap(Map, State->Considerations);

	for (const FStateTreeTransition& Trans : State->Transitions)
	{
		AddEditorNodesToMap(Map, Trans.Conditions);
	}

	for (const UStateTreeState* Child : State->Children)
	{
		AddStateNodesToMap(Map, Child);
	}
}

static TMap<FGuid, FString> BuildBindableStructNameMap(const UStateTreeEditorData* EditorData)
{
	TMap<FGuid, FString> Map;
	if (!EditorData) return Map;

	// Global evaluators and tasks
	AddEditorNodesToMap(Map, EditorData->Evaluators);
	AddEditorNodesToMap(Map, EditorData->GlobalTasks);

	// All state-level nodes (recursive)
	for (const UStateTreeState* SubTree : EditorData->SubTrees)
	{
		AddStateNodesToMap(Map, SubTree);
	}

	return Map;
}

/**
 * Gather property bindings for a specific editor node.
 * Returns a list of (TargetProperty, SourcePath) pairs.
 */
static TArray<FStateTreePropertyBindingAuditData> GatherNodeBindings(
	const UStateTreeEditorData* EditorData,
	const FGuid& NodeID,
	const TMap<FGuid, FString>& BindableNames)
{
	TArray<FStateTreePropertyBindingAuditData> Result;
	if (!EditorData) return Result;

	const FPropertyBindingBindingCollection* BindingCollection = EditorData->GetPropertyEditorBindings();
	if (!BindingCollection) return Result;

	TArray<const FPropertyBindingBinding*> Bindings;
	BindingCollection->GetBindingsFor(NodeID, Bindings);

	for (const FPropertyBindingBinding* Binding : Bindings)
	{
		const FPropertyBindingPath& SourcePath = Binding->GetSourcePath();
		const FPropertyBindingPath& TargetPath = Binding->GetTargetPath();

		// Target property name (on the node)
		const FString TargetProp = TargetPath.ToString();

		// Source: resolve struct name + property path
		FString SourceName;
		const FGuid SourceStructID = SourcePath.GetStructID();
		if (const FString* Found = BindableNames.Find(SourceStructID))
		{
			SourceName = *Found;
		}
		else
		{
			// Try to get name from EditorData's bindable struct descriptors
			TInstancedStruct<FPropertyBindingBindableStructDescriptor> Desc;
			if (EditorData->GetBindableStructByID(SourceStructID, Desc))
			{
				SourceName = Desc.Get().Name.ToString();
			}
		}

		const FString SourcePropPath = SourcePath.ToString();

		FString FullSource = SourceName;
		if (!SourcePropPath.IsEmpty())
		{
			if (!FullSource.IsEmpty()) FullSource += TEXT(".");
			FullSource += SourcePropPath;
		}

		if (!TargetProp.IsEmpty() && !FullSource.IsEmpty())
		{
			FStateTreePropertyBindingAuditData BindingData;
			BindingData.TargetProperty = TargetProp;
			BindingData.SourcePath = MoveTemp(FullSource);
			Result.Add(MoveTemp(BindingData));
		}
	}

	return Result;
}

// ---------------------------------------------------------------------------
// Editor node extraction
// ---------------------------------------------------------------------------

static FStateTreeEditorNodeAuditData GatherEditorNodeData(
	const FStateTreeEditorNode& EditorNode,
	const UStateTreeEditorData* EditorData,
	const TMap<FGuid, FString>& BindableNames)
{
	FStateTreeEditorNodeAuditData NodeData;
	NodeData.Name = EditorNode.GetName().ToString();
	NodeData.ExpressionOperand = ExpressionOperandToString(EditorNode.ExpressionOperand);

	// For Blueprint-based nodes, InstanceObject holds the actual BP class.
	// The Node struct is just the C++ wrapper (e.g. StateTreeBlueprintTaskWrapper).
	if (EditorNode.InstanceObject)
	{
		NodeData.ClassName = EditorNode.InstanceObject->GetClass()->GetName();
		NodeData.Properties = GatherObjectProperties(EditorNode.InstanceObject);
	}
	else
	{
		const FInstancedStruct& NodeStruct = EditorNode.Node;
		if (const UScriptStruct* ScriptStruct = NodeStruct.GetScriptStruct())
		{
			NodeData.ClassName = ScriptStruct->GetName();
		}
		NodeData.Properties = GatherStructProperties(EditorNode.Node);
	}

	// Property bindings
	NodeData.Bindings = GatherNodeBindings(EditorData, EditorNode.ID, BindableNames);

	return NodeData;
}

// ---------------------------------------------------------------------------
// Transition extraction
// ---------------------------------------------------------------------------

static FStateTreeTransitionAuditData GatherTransitionData(
	const FStateTreeTransition& Transition,
	const UStateTreeEditorData* EditorData,
	const TMap<FGuid, FString>& BindableNames)
{
	FStateTreeTransitionAuditData TransData;
	TransData.Trigger = TransitionTriggerToString(Transition.Trigger);
	TransData.bEnabled = Transition.bTransitionEnabled;
	TransData.bDelayTransition = Transition.bDelayTransition;
	TransData.DelayDuration = Transition.DelayDuration;
	TransData.DelayRandomVariance = Transition.DelayRandomVariance;
	TransData.Priority = PriorityToString(Transition.Priority);

#if WITH_EDITORONLY_DATA
	const FStateTreeStateLink& Link = Transition.State;
	if (Link.LinkType == EStateTreeTransitionType::GotoState)
	{
		TransData.TargetState = Link.Name.ToString();
	}
	else
	{
		TransData.TargetState = TransitionTypeToString(Link.LinkType);
	}
#endif

	for (const FStateTreeEditorNode& CondNode : Transition.Conditions)
	{
		TransData.Conditions.Add(GatherEditorNodeData(CondNode, EditorData, BindableNames));
	}
	if (TransData.Conditions.Num() > 0) TransData.Conditions[0].bIsFirstInList = true;

	return TransData;
}

// ---------------------------------------------------------------------------
// State extraction (recursive)
// ---------------------------------------------------------------------------

static FStateTreeStateAuditData GatherStateData(
	const UStateTreeState* State,
	const UStateTreeEditorData* EditorData,
	const TMap<FGuid, FString>& BindableNames)
{
	FStateTreeStateAuditData StateData;
	if (!State) return StateData;

	StateData.Name = State->Name.ToString();
	StateData.Type = StateTypeToString(State->Type);
	StateData.bEnabled = State->bEnabled;
	StateData.Description = State->Description;

	// Tag
	if (State->Tag.IsValid())
	{
		StateData.Tag = State->Tag.ToString();
	}

	// Selection behavior (only capture when non-default)
	if (State->SelectionBehavior != EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder)
	{
		StateData.SelectionBehavior = SelectionBehaviorToString(State->SelectionBehavior);
	}

	// Tasks completion type (only when not the default "All")
	if (State->TasksCompletion != EStateTreeTaskCompletionType::All)
	{
		StateData.TasksCompletion = TEXT("Any");
	}

	// Weight (only when non-default)
	StateData.Weight = State->Weight;

	// Custom tick rate
	StateData.bHasCustomTickRate = State->bHasCustomTickRate;
	if (State->bHasCustomTickRate)
	{
		StateData.CustomTickRate = State->CustomTickRate;
	}

	// Required event to enter
	if (State->bHasRequiredEventToEnter && State->RequiredEventToEnter.IsValid())
	{
		if (State->RequiredEventToEnter.Tag.IsValid())
		{
			StateData.RequiredEventTag = State->RequiredEventToEnter.Tag.ToString();
		}
		if (State->RequiredEventToEnter.PayloadStruct)
		{
			StateData.RequiredEventPayload = State->RequiredEventToEnter.PayloadStruct->GetName();
		}
		StateData.bConsumeEventOnSelect = State->RequiredEventToEnter.bConsumeEventOnSelect;
	}

	// Check prerequisites when activating child directly
	StateData.bCheckPrerequisitesWhenActivatingChildDirectly = State->bCheckPrerequisitesWhenActivatingChildDirectly;

	// Parameter overrides (for Linked/LinkedAsset states)
	if (State->Parameters.Parameters.IsValid())
	{
		const FInstancedPropertyBag& Params = State->Parameters.Parameters;
		const FConstStructView ParamView = Params.GetValue();
		if (ParamView.IsValid())
		{
			const UScriptStruct* ParamStruct = ParamView.GetScriptStruct();
			const uint8* ParamMemory = ParamView.GetMemory();
			if (ParamStruct && ParamMemory)
			{
				for (TFieldIterator<FProperty> PropIt(ParamStruct); PropIt; ++PropIt)
				{
					const FProperty* Prop = *PropIt;

					// Only include overridden parameters
					const FPropertyBagPropertyDesc* Desc = Params.FindPropertyDescByName(Prop->GetFName());
					if (!Desc || !State->IsParametersPropertyOverridden(Desc->ID))
					{
						continue;
					}

					FString Value;
					Prop->ExportText_Direct(Value, ParamMemory + Prop->GetOffset_ForInternal(), nullptr, nullptr, PPF_None);
					Value = ResolveEnumDisplayName(Prop, Value);
					Value = CleanDecimalValue(Value);

					if (!Value.IsEmpty() && Value != TEXT("()") && Value != TEXT("None") && !IsSentinelValue(Value))
					{
						FPropertyOverrideData Override;
						Override.Name = Prop->GetAuthoredName();
						Override.Value = MoveTemp(Value);
						StateData.ParameterOverrides.Add(MoveTemp(Override));
					}
				}
			}
		}
	}

	// Linked state info
	if (State->Type == EStateTreeStateType::Linked)
	{
#if WITH_EDITORONLY_DATA
		StateData.LinkedStateName = State->LinkedSubtree.Name.ToString();
#endif
	}
	else if (State->Type == EStateTreeStateType::LinkedAsset)
	{
		if (State->LinkedAsset)
		{
			StateData.LinkedAssetPath = State->LinkedAsset->GetPathName();
		}
	}

	// Tasks (array or SingleTask)
	if (State->Tasks.Num() > 0)
	{
		for (const FStateTreeEditorNode& TaskNode : State->Tasks)
		{
			StateData.Tasks.Add(GatherEditorNodeData(TaskNode, EditorData, BindableNames));
		}
	}
	else if (State->SingleTask.Node.IsValid())
	{
		StateData.Tasks.Add(GatherEditorNodeData(State->SingleTask, EditorData, BindableNames));
	}

	// Mark first item in each list
	if (StateData.Tasks.Num() > 0) StateData.Tasks[0].bIsFirstInList = true;

	// Enter conditions
	for (const FStateTreeEditorNode& CondNode : State->EnterConditions)
	{
		StateData.EnterConditions.Add(GatherEditorNodeData(CondNode, EditorData, BindableNames));
	}
	if (StateData.EnterConditions.Num() > 0) StateData.EnterConditions[0].bIsFirstInList = true;

	// Considerations (utility)
	for (const FStateTreeEditorNode& ConsNode : State->Considerations)
	{
		StateData.Considerations.Add(GatherEditorNodeData(ConsNode, EditorData, BindableNames));
	}
	if (StateData.Considerations.Num() > 0) StateData.Considerations[0].bIsFirstInList = true;

	// Transitions
	for (const FStateTreeTransition& Trans : State->Transitions)
	{
		StateData.Transitions.Add(GatherTransitionData(Trans, EditorData, BindableNames));
	}

	// Children (recursive)
	for (const UStateTreeState* Child : State->Children)
	{
		StateData.Children.Add(GatherStateData(Child, EditorData, BindableNames));
	}

	return StateData;
}

// ---------------------------------------------------------------------------
// GatherData
// ---------------------------------------------------------------------------

FStateTreeAuditData FStateTreeAuditor::GatherData(const UStateTree* ST)
{
	FStateTreeAuditData Data;
	if (!ST) return Data;

	Data.Name = ST->GetName();
	Data.Path = ST->GetPathName();
	Data.PackageName = ST->GetPackage()->GetName();
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	// Schema name
	if (const UStateTreeSchema* Schema = ST->GetSchema())
	{
		Data.SchemaName = Schema->GetClass()->GetName();
	}

#if WITH_EDITORONLY_DATA
	const UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!EditorData)
	{
		UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: StateTree %s has no EditorData"), *Data.Name);
		return Data;
	}

	// Build a GUID->Name map for binding source resolution
	const TMap<FGuid, FString> BindableNames = BuildBindableStructNameMap(EditorData);

	// Global evaluators
	for (const FStateTreeEditorNode& EvalNode : EditorData->Evaluators)
	{
		Data.Evaluators.Add(GatherEditorNodeData(EvalNode, EditorData, BindableNames));
	}
	if (Data.Evaluators.Num() > 0) Data.Evaluators[0].bIsFirstInList = true;

	// Global tasks
	for (const FStateTreeEditorNode& TaskNode : EditorData->GlobalTasks)
	{
		Data.GlobalTasks.Add(GatherEditorNodeData(TaskNode, EditorData, BindableNames));
	}
	if (Data.GlobalTasks.Num() > 0) Data.GlobalTasks[0].bIsFirstInList = true;

	// Sub-trees (root states)
	for (const UStateTreeState* SubTree : EditorData->SubTrees)
	{
		Data.SubTrees.Add(GatherStateData(SubTree, EditorData, BindableNames));
	}
#endif

	return Data;
}

// ---------------------------------------------------------------------------
// Markdown serialization helpers
// ---------------------------------------------------------------------------

static FString Indent(int32 Depth)
{
	FString Result;
	for (int32 i = 0; i < Depth; ++i)
		Result += TEXT("  ");
	return Result;
}

static void SerializeEditorNodes(FString& Out, const TArray<FStateTreeEditorNodeAuditData>& Nodes, int32 Depth, bool bShowOperand = false)
{
	for (const FStateTreeEditorNodeAuditData& Node : Nodes)
	{
		FString Label = Node.Name;
		if (!Node.ClassName.IsEmpty() && Node.ClassName != Node.Name)
		{
			Label += FString::Printf(TEXT(" (%s)"), *Node.ClassName);
		}
		// Only show operand for conditions (where AND vs OR matters), skip first item
		if (bShowOperand && !Node.bIsFirstInList && !Node.ExpressionOperand.IsEmpty())
		{
			Label = FString::Printf(TEXT("%s %s"), *Node.ExpressionOperand, *Label);
		}

		Out += FString::Printf(TEXT("%s- %s\n"), *Indent(Depth), *Label);

		for (const FPropertyOverrideData& Prop : Node.Properties)
		{
			Out += FString::Printf(TEXT("%s  %s: %s\n"), *Indent(Depth), *Prop.Name, *Prop.Value);
		}

		for (const FStateTreePropertyBindingAuditData& Binding : Node.Bindings)
		{
			Out += FString::Printf(TEXT("%s  [%s <- %s]\n"), *Indent(Depth), *Binding.TargetProperty, *Binding.SourcePath);
		}
	}
}

static void SerializeTransitions(FString& Out, const TArray<FStateTreeTransitionAuditData>& Transitions, int32 Depth)
{
	for (const FStateTreeTransitionAuditData& Trans : Transitions)
	{
		FString Line = FString::Printf(TEXT("%s -> %s"), *Trans.Trigger, *Trans.TargetState);

		if (Trans.Priority != TEXT("Normal") && Trans.Priority != TEXT("None"))
		{
			Line += FString::Printf(TEXT(" [%s]"), *Trans.Priority);
		}

		if (!Trans.bEnabled)
		{
			Line += TEXT(" [disabled]");
		}

		if (Trans.bDelayTransition)
		{
			FString Duration = CleanDecimalValue(FString::Printf(TEXT("%.2f"), Trans.DelayDuration));
			Line += FString::Printf(TEXT(" (delay %ss"), *Duration);
			if (Trans.DelayRandomVariance > 0.f)
			{
				FString Variance = CleanDecimalValue(FString::Printf(TEXT("%.2f"), Trans.DelayRandomVariance));
				Line += FString::Printf(TEXT(" +/- %ss"), *Variance);
			}
			Line += TEXT(")");
		}

		Out += FString::Printf(TEXT("%s- %s\n"), *Indent(Depth), *Line);

		if (!Trans.Conditions.IsEmpty())
		{
			Out += FString::Printf(TEXT("%sConditions:\n"), *Indent(Depth + 1));
			SerializeEditorNodes(Out, Trans.Conditions, Depth + 2, /*bShowOperand=*/ true);
		}
	}
}

static void SerializeState(FString& Out, const FStateTreeStateAuditData& State, int32 Depth)
{
	// State header
	FString Header = FString::Printf(TEXT("%s (%s)"), *State.Name, *State.Type);
	if (!State.bEnabled)
	{
		Header += TEXT(" [disabled]");
	}

	// Use markdown headings for the first few levels, then indented bold text
	const int32 HeadingLevel = Depth + 1; // # for root states, ## for children, etc.
	if (HeadingLevel <= 6)
	{
		FString Hashes;
		for (int32 i = 0; i < HeadingLevel; ++i)
			Hashes += TEXT("#");
		Out += FString::Printf(TEXT("%s %s\n\n"), *Hashes, *Header);
	}
	else
	{
		Out += FString::Printf(TEXT("%s**%s**\n\n"), *Indent(Depth - 6), *Header);
	}

	if (!State.Description.IsEmpty())
	{
		Out += FString::Printf(TEXT("%s\n\n"), *State.Description);
	}

	// State metadata (only non-default values)
	if (!State.Tag.IsEmpty())
	{
		Out += FString::Printf(TEXT("Tag: %s\n"), *State.Tag);
	}
	if (!State.SelectionBehavior.IsEmpty())
	{
		Out += FString::Printf(TEXT("Selection Behavior: %s\n"), *State.SelectionBehavior);
	}
	if (!State.TasksCompletion.IsEmpty())
	{
		Out += FString::Printf(TEXT("Tasks Completion: %s\n"), *State.TasksCompletion);
	}
	if (State.Weight != 1.f)
	{
		Out += FString::Printf(TEXT("Weight: %s\n"), *CleanDecimalValue(FString::Printf(TEXT("%.2f"), State.Weight)));
	}
	if (State.bHasCustomTickRate)
	{
		Out += FString::Printf(TEXT("Custom Tick Rate: %s\n"), *CleanDecimalValue(FString::Printf(TEXT("%.2f"), State.CustomTickRate)));
	}
	if (!State.RequiredEventTag.IsEmpty())
	{
		Out += FString::Printf(TEXT("Required Event: %s\n"), *State.RequiredEventTag);
		if (!State.RequiredEventPayload.IsEmpty())
		{
			Out += FString::Printf(TEXT("  Payload: %s\n"), *State.RequiredEventPayload);
		}
		if (!State.bConsumeEventOnSelect)
		{
			Out += TEXT("  Consume Event On Select: False\n");
		}
	}
	if (!State.bCheckPrerequisitesWhenActivatingChildDirectly)
	{
		Out += TEXT("Check Prerequisites When Activating Child Directly: False\n");
	}

	// Linked state info
	if (!State.LinkedStateName.IsEmpty())
	{
		Out += FString::Printf(TEXT("Linked to: %s\n"), *State.LinkedStateName);
	}
	if (!State.LinkedAssetPath.IsEmpty())
	{
		Out += FString::Printf(TEXT("Linked asset: %s\n"), *State.LinkedAssetPath);
	}

	// Parameter overrides
	if (!State.ParameterOverrides.IsEmpty())
	{
		Out += TEXT("Parameter Overrides:\n");
		for (const FPropertyOverrideData& Param : State.ParameterOverrides)
		{
			Out += FString::Printf(TEXT("  %s: %s\n"), *Param.Name, *Param.Value);
		}
	}

	// Add blank line after metadata block if any metadata was written
	if (!State.Tag.IsEmpty() || !State.SelectionBehavior.IsEmpty() || !State.TasksCompletion.IsEmpty()
		|| State.Weight != 1.f || State.bHasCustomTickRate || !State.RequiredEventTag.IsEmpty()
		|| !State.bCheckPrerequisitesWhenActivatingChildDirectly
		|| !State.LinkedStateName.IsEmpty() || !State.LinkedAssetPath.IsEmpty()
		|| !State.ParameterOverrides.IsEmpty())
	{
		Out += TEXT("\n");
	}

	// Tasks
	if (!State.Tasks.IsEmpty())
	{
		Out += TEXT("Tasks:\n");
		SerializeEditorNodes(Out, State.Tasks, 0);
		Out += TEXT("\n");
	}

	// Enter conditions
	if (!State.EnterConditions.IsEmpty())
	{
		Out += TEXT("Enter Conditions:\n");
		SerializeEditorNodes(Out, State.EnterConditions, 0, /*bShowOperand=*/ true);
		Out += TEXT("\n");
	}

	// Considerations
	if (!State.Considerations.IsEmpty())
	{
		Out += TEXT("Considerations:\n");
		SerializeEditorNodes(Out, State.Considerations, 0);
		Out += TEXT("\n");
	}

	// Transitions
	if (!State.Transitions.IsEmpty())
	{
		Out += TEXT("Transitions:\n");
		SerializeTransitions(Out, State.Transitions, 0);
		Out += TEXT("\n");
	}

	// Children
	for (const FStateTreeStateAuditData& Child : State.Children)
	{
		SerializeState(Out, Child, Depth + 1);
	}
}

// ---------------------------------------------------------------------------
// SerializeToMarkdown
// ---------------------------------------------------------------------------

FString FStateTreeAuditor::SerializeToMarkdown(const FStateTreeAuditData& Data)
{
	FString Out;

	// Header
	Out += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Out += FString::Printf(TEXT("Path: %s\n"), *Data.Path);
	Out += TEXT("Type: StateTree\n");

	const FString Hash = FAuditFileUtils::ComputeFileHash(Data.SourceFilePath);
	Out += FString::Printf(TEXT("Hash: %s\n"), *Hash);

	if (!Data.SchemaName.IsEmpty())
	{
		Out += FString::Printf(TEXT("Schema: %s\n"), *Data.SchemaName);
	}

	// Global evaluators
	if (!Data.Evaluators.IsEmpty())
	{
		Out += TEXT("\n## Global Evaluators\n\n");
		SerializeEditorNodes(Out, Data.Evaluators, 0);
	}

	// Global tasks
	if (!Data.GlobalTasks.IsEmpty())
	{
		Out += TEXT("\n## Global Tasks\n\n");
		SerializeEditorNodes(Out, Data.GlobalTasks, 0);
	}

	// State hierarchy (root states start at h1, no section header)
	if (!Data.SubTrees.IsEmpty())
	{
		Out += TEXT("\n");
		for (const FStateTreeStateAuditData& SubTree : Data.SubTrees)
		{
			SerializeState(Out, SubTree, 0);
		}
	}

	return Out;
}

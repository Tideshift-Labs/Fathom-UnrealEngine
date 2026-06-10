#include "PCGGraphAuditor.h"

#include "Audit/AuditFileUtils.h"
#include "Audit/AuditHelpers.h"
#include "FathomUELinkModule.h"
#include "Misc/EngineVersionComparison.h"

#include "PCGCommon.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "Elements/PCGReroute.h"
#include "StructUtils/PropertyBag.h"

namespace
{

/**
 * Reroute-family check by settings class rather than GetType(): the class test
 * also covers named reroute declaration/usage subclasses, and does not depend
 * on the WITH_EDITOR-only GetType() override on UPCGRerouteSettings.
 */
bool IsRerouteFamilyNode(const UPCGNode* Node)
{
	const UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
	return Settings && Settings->IsA<UPCGRerouteSettings>();
}

/**
 * Settings type name via enum reflection rather than a switch: enumerators
 * added in newer engine versions stringify automatically instead of needing
 * version-guarded case labels.
 * GetType() is WITH_EDITOR-only in the PCG module; this module is Editor-type
 * so it is always available here.
 */
FString SettingsTypeToString(const UPCGSettings* Settings)
{
	const UEnum* TypeEnum = StaticEnum<EPCGSettingsType>();
	const FString Name = TypeEnum ? TypeEnum->GetNameStringByValue(static_cast<int64>(Settings->GetType())) : FString();
	return Name.IsEmpty() ? FString(TEXT("Generic")) : Name;
}

/**
 * Stringify a pin's allowed types. The AllowedTypes field changed type across
 * engine versions: EPCGDataType enum before 5.7, FPCGDataTypeIdentifier struct
 * in 5.7+. The struct has an exported ToString() that yields clean names like
 * "Surface" or "Point | Spline"; the raw reflection export is unreadable for
 * it, so it is only the fallback for the older enum form.
 */
FString FormatAllowedTypes(const FPCGPinProperties& Properties)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
	return Properties.AllowedTypes.ToString();
#else
	static const FProperty* AllowedTypesProp =
		FPCGPinProperties::StaticStruct()->FindPropertyByName(TEXT("AllowedTypes"));
	if (!AllowedTypesProp)
	{
		return FString();
	}

	FString Exported;
	AllowedTypesProp->ExportText_Direct(
		Exported, AllowedTypesProp->ContainerPtrToValuePtr<void>(&Properties), nullptr, nullptr, PPF_None);
	return FathomAuditHelpers::CleanExportedValue(Exported);
#endif
}

void GatherPins(const TArray<TObjectPtr<UPCGPin>>& Pins, const TCHAR* Direction, TArray<FPCGPinAuditData>& Out)
{
	for (const UPCGPin* Pin : Pins)
	{
		// Advanced pins include the auto-generated override/user-param pins,
		// which just mirror the settings values already captured per node.
		if (!Pin || Pin->Properties.bInvisiblePin || Pin->Properties.IsAdvancedPin())
		{
			continue;
		}

		FPCGPinAuditData PinData;
		PinData.Label = Pin->Properties.Label.ToString();
		PinData.Direction = Direction;
		PinData.AllowedTypes = FormatAllowedTypes(Pin->Properties);
		PinData.bAllowMultipleData = Pin->Properties.bAllowMultipleData;
		Out.Add(MoveTemp(PinData));
	}
}

/** Capture the values of a node's overridable params, where the procedural intent lives. */
void GatherSettingsValues(const UPCGSettings* Settings, TArray<FPropertyOverrideData>& Out)
{
	const UObject* DefaultSettings = Settings->GetClass()->GetDefaultObject();

	for (const FPCGSettingsOverridableParam& Param : Settings->OverridableParams())
	{
		// Params can reference properties owned by other structs entirely: subgraph
		// nodes expose the subgraph's user parameters as override pins, and Blueprint
		// elements expose properties of the Blueprint instance. Reading those chains
		// against the settings object dereferences garbage memory, so only accept
		// chains whose head property is owned by this settings class. Skipped params
		// are not lost: their values are audited on the referenced asset itself or
		// captured through the instanced subobject gather below.
		const FProperty* LeafProp = nullptr;
		const void* Container = Settings;
		const void* DefaultContainer = DefaultSettings;
		if (!Param.Properties.IsEmpty() && !Param.Properties.Contains(nullptr)
			&& Param.Properties[0]->GetOwnerStruct()
			&& Settings->GetClass()->IsChildOf(Param.Properties[0]->GetOwnerStruct()))
		{
			LeafProp = Param.Properties.Last();
			for (int32 i = 0; i < Param.Properties.Num() - 1; ++i)
			{
				const FProperty* Link = Param.Properties[i];
				if (!Link->IsA<FStructProperty>())
				{
					// Only plain struct chains can be walked by container offset.
					LeafProp = nullptr;
					break;
				}
				Container = Link->ContainerPtrToValuePtr<void>(Container);
				DefaultContainer = Link->ContainerPtrToValuePtr<void>(DefaultContainer);
			}
		}
		if (!LeafProp && Param.PropertiesNames.Num() == 1)
		{
			// Fallback when the transient chain was not populated on load.
			LeafProp = FindFProperty<FProperty>(Settings->GetClass(), Param.PropertiesNames[0]);
			Container = Settings;
			DefaultContainer = DefaultSettings;
		}
		if (!LeafProp || FathomAuditHelpers::HasBrokenTypeMetadata(LeafProp))
		{
			continue;
		}

		// Suppress values that match the class defaults, following the
		// only-overrides convention of the other auditors. Authored values
		// (including per-node seeds) survive; boilerplate does not.
		const void* ValuePtr = LeafProp->ContainerPtrToValuePtr<void>(Container);
		const void* DefaultPtr = LeafProp->ContainerPtrToValuePtr<void>(DefaultContainer);
		if (LeafProp->Identical(ValuePtr, DefaultPtr))
		{
			continue;
		}

		FString Value = FathomAuditHelpers::FormatPropertyValue(LeafProp, ValuePtr, 0);
		if (Value.IsEmpty())
		{
			continue;
		}

		FPropertyOverrideData Entry;
		// GetPropertyPath() is not exported from the PCG module; join the names locally.
		Entry.Name = Param.bHasNameClash
			? FString::JoinBy(Param.PropertiesNames, TEXT("/"), [](const FName& PropName) { return PropName.ToString(); })
			: Param.Label.ToString();
		Entry.Value = MoveTemp(Value);
		Out.Add(MoveTemp(Entry));
	}
}

/**
 * Capture instanced subobject properties (e.g. a spawner's MeshSelectorParameters
 * with its mesh list, or InstanceDataPackerParameters). These hold key node
 * configuration but are not overridable params, so the depth-B gather misses
 * them. FormatPropertyValue expands instanced subobjects with CDO filtering.
 */
void GatherInstancedSubobjectValues(const UPCGSettings* Settings, TArray<FPropertyOverrideData>& Out)
{
	for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It)
	{
		const FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_PersistentInstance | CPF_ContainsInstancedReference)
			|| Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
			|| !Prop->HasAnyPropertyFlags(CPF_Edit)
			|| FathomAuditHelpers::HasBrokenTypeMetadata(Prop))
		{
			continue;
		}

		FString Value = FathomAuditHelpers::FormatPropertyValue(
			Prop, Prop->ContainerPtrToValuePtr<void>(Settings), 0);
		if (Value.IsEmpty() || Value == TEXT("None") || Value == TEXT("()"))
		{
			continue;
		}

		FPropertyOverrideData Entry;
		Entry.Name = Prop->GetAuthoredName();
		Entry.Value = MoveTemp(Value);
		Out.Add(MoveTemp(Entry));
	}
}

/**
 * Resolve a graph interface to its concrete UPCGGraph by walking the instance
 * chain locally. UPCGGraphInterface::GetGraph() recurses through chained
 * instances with no cycle guard; editing prevents cyclic chains but corrupted
 * assets can still hold them (docs/learnings/defensive-asset-traversal.md).
 */
const UPCGGraph* ResolveConcreteGraph(const UPCGGraphInterface* Interface)
{
	TSet<const UPCGGraphInterface*> Visited;
	const UPCGGraphInterface* Current = Interface;
	while (Current)
	{
		bool bAlreadyVisited = false;
		Visited.Add(Current, &bAlreadyVisited);
		if (bAlreadyVisited)
		{
			return nullptr;
		}
		if (const UPCGGraph* Graph = Cast<UPCGGraph>(Current))
		{
			return Graph;
		}
		const UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(Current);
		Current = Instance ? Instance->Graph.Get() : nullptr;
	}
	return nullptr;
}

/** Read user parameters out of a property bag; marks per-parameter overrides for graph instances. */
void GatherBagParameters(const FInstancedPropertyBag* Bag, const UPCGGraphInstance* InstanceForOverrides, TArray<FPCGGraphParamData>& Out)
{
	if (!Bag || !Bag->IsValid())
	{
		return;
	}
	const UPropertyBag* BagStruct = Bag->GetPropertyBagStruct();
	const uint8* Memory = Bag->GetValue().GetMemory();
	if (!BagStruct || !Memory)
	{
		return;
	}

	for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
	{
		FPCGGraphParamData Param;
		Param.Name = Desc.Name.ToString();

		// Bag parameters typed to a deleted user struct/enum/class keep a broken
		// FProperty. Emit a placeholder rather than skipping: the parameter name
		// is still referenced by subgraph override pins.
		if (!Desc.CachedProperty || FathomAuditHelpers::HasBrokenTypeMetadata(Desc.CachedProperty))
		{
			Param.Type = Desc.CachedProperty ? FathomAuditHelpers::GetSafeCPPType(Desc.CachedProperty) : TEXT("Unknown");
			Param.Value = TEXT("(unavailable)");
			Out.Add(MoveTemp(Param));
			continue;
		}

		Param.Type = FathomAuditHelpers::GetSafeCPPType(Desc.CachedProperty);
		Param.Value = FathomAuditHelpers::FormatPropertyValue(
			Desc.CachedProperty, Desc.CachedProperty->ContainerPtrToValuePtr<void>(Memory), 0);
		if (InstanceForOverrides)
		{
			Param.bOverridden = InstanceForOverrides->IsPropertyOverridden(Desc.CachedProperty);
		}
		Out.Add(MoveTemp(Param));
	}
}

/**
 * Follow downstream edges through reroute nodes to real endpoint pins.
 * Named reroutes need no special casing: a declaration links to its usage
 * nodes via real edges on an invisible output pin, and usage nodes are
 * themselves reroute-family, so recursion continues to real consumers.
 */
void TraceToRealTargets(
	const UPCGPin* FromPin,
	const TMap<const UPCGNode*, int32>& NodeIdMap,
	TSet<const UPCGNode*>& VisitedReroutes,
	TArray<const UPCGPin*>& OutTargets)
{
	for (const UPCGEdge* Edge : FromPin->Edges)
	{
		const UPCGPin* DownPin = Edge ? Edge->OutputPin.Get() : nullptr;
		if (!DownPin || DownPin == FromPin)
		{
			continue;
		}
		const UPCGNode* DownNode = DownPin->Node;
		if (!DownNode)
		{
			continue;
		}

		if (IsRerouteFamilyNode(DownNode))
		{
			bool bAlreadyVisited = false;
			VisitedReroutes.Add(DownNode, &bAlreadyVisited);
			if (bAlreadyVisited)
			{
				continue;
			}
			for (const UPCGPin* ReroutePin : DownNode->GetOutputPins())
			{
				if (ReroutePin)
				{
					TraceToRealTargets(ReroutePin, NodeIdMap, VisitedReroutes, OutTargets);
				}
			}
		}
		else if (NodeIdMap.Contains(DownNode))
		{
			OutTargets.Add(DownPin);
		}
	}
}

FString SanitizeTableCell(const FString& In)
{
	FString Out = In;
	Out.ReplaceInline(TEXT("\r"), TEXT(""));
	Out.ReplaceInline(TEXT("\n"), TEXT(" / "));
	Out.ReplaceInline(TEXT("|"), TEXT("\\|"));
	return Out;
}

void SerializeHeader(FString& Out, const FString& Name, const FString& Path)
{
	Out += FString::Printf(TEXT("# %s\n"), *Name);
	Out += FString::Printf(TEXT("Path: %s\n"), *Path);
	Out += TEXT("Type: PCG\n");
}

void SerializeSourceAndHash(FString& Out, const FString& SourceFilePath)
{
	if (!SourceFilePath.IsEmpty())
	{
		Out += FString::Printf(TEXT("SourcePath: %s\n"), *FAuditFileUtils::ToProjectRelativeSourcePath(SourceFilePath));
	}
	Out += FString::Printf(TEXT("Hash: %s\n"), *FAuditFileUtils::ComputeFileHash(SourceFilePath));
}

void SerializeParameterTable(FString& Out, const TArray<FPCGGraphParamData>& Parameters, bool bWithOverrideColumn)
{
	if (Parameters.IsEmpty())
	{
		return;
	}

	Out += TEXT("\n## Parameters\n\n");
	if (bWithOverrideColumn)
	{
		Out += TEXT("| Name | Type | Value | Overridden |\n");
		Out += TEXT("|------|------|-------|------------|\n");
	}
	else
	{
		Out += TEXT("| Name | Type | Default |\n");
		Out += TEXT("|------|------|---------|\n");
	}

	for (const FPCGGraphParamData& Param : Parameters)
	{
		if (bWithOverrideColumn)
		{
			Out += FString::Printf(TEXT("| %s | %s | %s | %s |\n"),
				*SanitizeTableCell(Param.Name),
				*SanitizeTableCell(Param.Type),
				*SanitizeTableCell(Param.Value),
				Param.bOverridden ? TEXT("yes") : TEXT(""));
		}
		else
		{
			Out += FString::Printf(TEXT("| %s | %s | %s |\n"),
				*SanitizeTableCell(Param.Name),
				*SanitizeTableCell(Param.Type),
				*SanitizeTableCell(Param.Value));
		}
	}
}

FString BuildPinList(const TArray<FPCGPinAuditData>& Pins, const TCHAR* Direction)
{
	FString List;
	for (const FPCGPinAuditData& Pin : Pins)
	{
		if (Pin.Direction != Direction)
		{
			continue;
		}
		if (!List.IsEmpty())
		{
			List += TEXT(", ");
		}
		List += Pin.Label;

		FString Qualifiers = Pin.AllowedTypes;
		if (!Pin.bAllowMultipleData)
		{
			Qualifiers += Qualifiers.IsEmpty() ? TEXT("single") : TEXT(", single");
		}
		if (!Qualifiers.IsEmpty())
		{
			List += FString::Printf(TEXT(" (%s)"), *Qualifiers);
		}
	}
	return List;
}

} // namespace

// ---------------------------------------------------------------------------
// GatherData
// ---------------------------------------------------------------------------

FPCGGraphAuditData FPCGGraphAuditor::GatherData(const UPCGGraph* Graph)
{
	FPCGGraphAuditData Data;
	if (!Graph)
	{
		return Data;
	}

	Data.Name = Graph->GetName();
	Data.Path = Graph->GetPathName();
	Data.PackageName = Graph->GetPackage()->GetName();
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	GatherBagParameters(Graph->GetUserParametersStruct(), nullptr, Data.Parameters);

	// Node list: Input first, Output last, reroutes excluded (edges trace through them).
	const UPCGNode* InputNode = Graph->GetInputNode();
	const UPCGNode* OutputNode = Graph->GetOutputNode();

	TMap<const UPCGNode*, int32> NodeIdMap;
	TArray<const UPCGNode*> OrderedNodes;
	auto AddNode = [&NodeIdMap, &OrderedNodes](const UPCGNode* Node)
	{
		if (Node && !NodeIdMap.Contains(Node))
		{
			NodeIdMap.Add(Node, OrderedNodes.Num());
			OrderedNodes.Add(Node);
		}
	};
	AddNode(InputNode);
	for (const UPCGNode* Node : Graph->GetNodes())
	{
		if (Node && Node != InputNode && Node != OutputNode && !IsRerouteFamilyNode(Node))
		{
			AddNode(Node);
		}
	}
	AddNode(OutputNode);

	Data.Nodes.Reserve(OrderedNodes.Num());
	for (const UPCGNode* Node : OrderedNodes)
	{
		FPCGNodeAuditData NodeData;
		NodeData.Id = NodeIdMap[Node];
		NodeData.Title = Node->GetNodeTitle(EPCGNodeTitleType::FullTitle).ToString();

		if (const UPCGSettings* Settings = Node->GetSettings())
		{
			NodeData.Type = SettingsTypeToString(Settings);
			NodeData.SettingsClass = Settings->GetClass()->GetName();
			GatherSettingsValues(Settings, NodeData.Settings);
			GatherInstancedSubobjectValues(Settings, NodeData.Settings);
		}
		else
		{
			NodeData.Type = TEXT("Unknown");
		}

		if (const UPCGBaseSubgraphNode* SubNode = Cast<UPCGBaseSubgraphNode>(Node))
		{
			NodeData.bIsSubgraph = true;
			if (const UPCGGraph* Subgraph = ResolveConcreteGraph(SubNode->GetSubgraphInterface()))
			{
				NodeData.SubgraphPath = Subgraph->GetPathName();
			}
			// When the node references a graph instance asset, record it too:
			// its own audit file carries the parameter overrides.
			if (const UPCGGraphInstance* OwnedInstance = Cast<UPCGGraphInstance>(SubNode->GetSubgraphInterface()))
			{
				if (const UPCGGraphInstance* InstanceAsset = Cast<UPCGGraphInstance>(OwnedInstance->Graph))
				{
					NodeData.SubgraphInstancePath = InstanceAsset->GetPathName();
				}
			}
		}

		GatherPins(Node->GetInputPins(), TEXT("Input"), NodeData.Pins);
		GatherPins(Node->GetOutputPins(), TEXT("Output"), NodeData.Pins);

		Data.Nodes.Add(MoveTemp(NodeData));
	}

	// Edges, traced through reroute nodes to real endpoints.
	for (const UPCGNode* Node : OrderedNodes)
	{
		for (const UPCGPin* OutPin : Node->GetOutputPins())
		{
			if (!OutPin)
			{
				continue;
			}
			TSet<const UPCGNode*> VisitedReroutes;
			TArray<const UPCGPin*> Targets;
			TraceToRealTargets(OutPin, NodeIdMap, VisitedReroutes, Targets);
			for (const UPCGPin* TargetPin : Targets)
			{
				FPCGEdgeAuditData EdgeData;
				EdgeData.SourceNodeId = NodeIdMap[Node];
				EdgeData.SourcePinLabel = OutPin->Properties.Label.ToString();
				EdgeData.TargetNodeId = NodeIdMap[TargetPin->Node];
				EdgeData.TargetPinLabel = TargetPin->Properties.Label.ToString();
				Data.Edges.Add(MoveTemp(EdgeData));
			}
		}
	}

	return Data;
}

// ---------------------------------------------------------------------------
// GatherInstanceData
// ---------------------------------------------------------------------------

FPCGGraphInstanceAuditData FPCGGraphAuditor::GatherInstanceData(const UPCGGraphInstance* Instance)
{
	FPCGGraphInstanceAuditData Data;
	if (!Instance)
	{
		return Data;
	}

	Data.Name = Instance->GetName();
	Data.Path = Instance->GetPathName();
	Data.PackageName = Instance->GetPackage()->GetName();
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	if (const UPCGGraphInterface* Parent = Instance->Graph)
	{
		Data.ParentGraphPath = Parent->GetPathName();
		if (const UPCGGraph* BaseGraph = ResolveConcreteGraph(Parent))
		{
			if (BaseGraph != Parent)
			{
				Data.BaseGraphPath = BaseGraph->GetPathName();
			}
		}
	}

	GatherBagParameters(Instance->GetUserParametersStruct(), Instance, Data.Parameters);

	return Data;
}

// ---------------------------------------------------------------------------
// SerializeToMarkdown
// ---------------------------------------------------------------------------

FString FPCGGraphAuditor::SerializeToMarkdown(const FPCGGraphAuditData& Data)
{
	FString Out;

	SerializeHeader(Out, Data.Name, Data.Path);
	SerializeSourceAndHash(Out, Data.SourceFilePath);

	SerializeParameterTable(Out, Data.Parameters, /*bWithOverrideColumn=*/false);

	if (!Data.Nodes.IsEmpty())
	{
		Out += TEXT("\n## Nodes\n\n");
		Out += TEXT("| Id | Type | Title | Settings |\n");
		Out += TEXT("|----|------|-------|----------|\n");
		for (const FPCGNodeAuditData& Node : Data.Nodes)
		{
			Out += FString::Printf(TEXT("| %d | %s | %s | %s |\n"),
				Node.Id,
				*SanitizeTableCell(Node.Type),
				*SanitizeTableCell(Node.Title),
				*SanitizeTableCell(Node.SettingsClass));
		}
	}

	FString Details;
	for (const FPCGNodeAuditData& Node : Data.Nodes)
	{
		if (Node.Settings.IsEmpty() && !Node.bIsSubgraph && Node.Pins.IsEmpty())
		{
			continue;
		}

		Details += FString::Printf(TEXT("\n### %d: %s\n"), Node.Id, *SanitizeTableCell(Node.Title));

		const FString InPins = BuildPinList(Node.Pins, TEXT("Input"));
		const FString OutPins = BuildPinList(Node.Pins, TEXT("Output"));
		if (!InPins.IsEmpty())
		{
			Details += FString::Printf(TEXT("Input Pins: %s\n"), *InPins);
		}
		if (!OutPins.IsEmpty())
		{
			Details += FString::Printf(TEXT("Output Pins: %s\n"), *OutPins);
		}

		if (!Node.SubgraphPath.IsEmpty())
		{
			Details += FString::Printf(TEXT("Subgraph: %s\n"), *Node.SubgraphPath);
		}
		if (!Node.SubgraphInstancePath.IsEmpty())
		{
			Details += FString::Printf(TEXT("SubgraphInstance: %s\n"), *Node.SubgraphInstancePath);
		}

		FathomAuditHelpers::FPropertyRenderStyle PropStyle;
		PropStyle.Indent = TEXT("");
		PropStyle.bUseBullet = true;
		PropStyle.InlineSeparator = TEXT(": ");
		FathomAuditHelpers::SerializePropertyOverridesToMarkdown(Details, Node.Settings, PropStyle);
	}
	if (!Details.IsEmpty())
	{
		Out += TEXT("\n## Node Details\n");
		Out += Details;
	}

	if (!Data.Edges.IsEmpty())
	{
		Out += TEXT("\n## Edges\n\n");
		for (const FPCGEdgeAuditData& Edge : Data.Edges)
		{
			Out += FString::Printf(TEXT("%d.%s -> %d.%s\n"),
				Edge.SourceNodeId, *Edge.SourcePinLabel, Edge.TargetNodeId, *Edge.TargetPinLabel);
		}
	}

	return Out;
}

// ---------------------------------------------------------------------------
// SerializeInstanceToMarkdown
// ---------------------------------------------------------------------------

FString FPCGGraphAuditor::SerializeInstanceToMarkdown(const FPCGGraphInstanceAuditData& Data)
{
	FString Out;

	SerializeHeader(Out, Data.Name, Data.Path);
	if (!Data.ParentGraphPath.IsEmpty())
	{
		Out += FString::Printf(TEXT("InstanceOf: %s\n"), *Data.ParentGraphPath);
	}
	if (!Data.BaseGraphPath.IsEmpty())
	{
		Out += FString::Printf(TEXT("BaseGraph: %s\n"), *Data.BaseGraphPath);
	}
	SerializeSourceAndHash(Out, Data.SourceFilePath);

	SerializeParameterTable(Out, Data.Parameters, /*bWithOverrideColumn=*/true);

	return Out;
}

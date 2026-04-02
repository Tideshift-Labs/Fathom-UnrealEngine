#include "Audit/BehaviorTreeAuditor.h"

#include "Audit/AuditFileUtils.h"
#include "Audit/AuditHelpers.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Decorators/BTDecorator_BlackboardBase.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Get a human-readable type string for a blackboard key. */
static FString GetBlackboardKeyTypeString(const UBlackboardKeyType* KeyType)
{
	if (!KeyType) return TEXT("Unknown");

	FString TypeName = KeyType->GetClass()->GetName();
	// Strip "BlackboardKeyType_" prefix
	static const FString Prefix = TEXT("BlackboardKeyType_");
	if (TypeName.StartsWith(Prefix))
	{
		TypeName.RightChopInline(Prefix.Len());
	}

	// Append metadata for Object, Class, and Enum types
	if (const auto* ObjKey = Cast<UBlackboardKeyType_Object>(KeyType))
	{
		if (ObjKey->BaseClass)
			return FString::Printf(TEXT("Object: %s"), *ObjKey->BaseClass->GetName());
	}
	else if (const auto* ClsKey = Cast<UBlackboardKeyType_Class>(KeyType))
	{
		if (ClsKey->BaseClass)
			return FString::Printf(TEXT("Class: %s"), *ClsKey->BaseClass->GetName());
	}
	else if (const auto* EnumKey = Cast<UBlackboardKeyType_Enum>(KeyType))
	{
		if (EnumKey->EnumType)
			return FString::Printf(TEXT("Enum: %s"), *EnumKey->EnumType->GetName());
		if (!EnumKey->EnumName.IsEmpty())
			return FString::Printf(TEXT("Enum: %s"), *EnumKey->EnumName);
	}
	else if (const auto* NEnumKey = Cast<UBlackboardKeyType_NativeEnum>(KeyType))
	{
		if (NEnumKey->EnumType)
			return FString::Printf(TEXT("Enum: %s"), *NEnumKey->EnumType->GetName());
		if (!NEnumKey->EnumName.IsEmpty())
			return FString::Printf(TEXT("Enum: %s"), *NEnumKey->EnumName);
	}

	return TypeName;
}

/** Get FlowAbortMode as a string. */
static FString GetAbortModeString(EBTFlowAbortMode::Type Mode)
{
	switch (Mode)
	{
	case EBTFlowAbortMode::None:          return TEXT("None");
	case EBTFlowAbortMode::LowerPriority: return TEXT("LowerPriority");
	case EBTFlowAbortMode::Self:          return TEXT("Self");
	case EBTFlowAbortMode::Both:          return TEXT("Both");
	default:                              return TEXT("Unknown");
	}
}

/** Get a short class name suitable for display (strip "BTTask_", "BTDecorator_", etc. prefixes for readability). */
static FString GetShortNodeClassName(const UBTNode* Node)
{
	if (!Node) return TEXT("Unknown");
	return Node->GetClass()->GetName();
}

// ---------------------------------------------------------------------------
// Generic UPROPERTY dump for BT nodes
// ---------------------------------------------------------------------------

/**
 * Collect all editor-visible (EditAnywhere/EditInstanceOnly) properties
 * from a BT node, skipping base UBTNode/UObject properties and transients.
 * Exports each property's value as a cleaned string.
 */
static TArray<FPropertyOverrideData> GatherNodeProperties(const UBTNode* Node, const UClass* StopAtClass = UBTNode::StaticClass())
{
	TArray<FPropertyOverrideData> Props;
	if (!Node) return Props;

	for (TFieldIterator<FProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		const FProperty* Prop = *PropIt;

		// Skip properties owned by the base engine classes
		const UClass* Owner = Prop->GetOwner<UClass>();
		if (Owner && Owner->IsChildOf(StopAtClass) && Owner != Node->GetClass())
		{
			// Allow properties from the node's own class and its game subclasses,
			// but skip properties defined in UBTNode, UBTTaskNode, UBTDecorator, etc.
			if (Owner == UBTNode::StaticClass() ||
				Owner == UBTTaskNode::StaticClass() ||
				Owner == UBTDecorator::StaticClass() ||
				Owner == UBTService::StaticClass() ||
				Owner == UBTAuxiliaryNode::StaticClass() ||
				Owner == UBTCompositeNode::StaticClass() ||
				Owner == UObject::StaticClass())
			{
				continue;
			}
		}

		// Only include editor-visible properties
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
			continue;

		// Skip transient and deprecated
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			continue;

		// Skip inline edit condition toggles (they're just bool flags for conditional visibility)
		if (Prop->HasMetaData(TEXT("InlineEditConditionToggle")))
			continue;

		// Skip known noise properties from engine BT base classes
		// (BTTask_BlueprintBase, BTDecorator_BlueprintBase, BTService_BlueprintBase)
		static const TSet<FString> SkipNames = {
			TEXT("TickInterval"), TEXT("bShowPropertyDetails"), TEXT("CustomDescription"),
		};
		if (SkipNames.Contains(Prop->GetName()))
			continue;

		FPropertyOverrideData Override;
		Override.Name = Prop->GetAuthoredName();
		Prop->ExportText_InContainer(0, Override.Value, Node, nullptr, nullptr, PPF_None);
		Override.Value = FathomAuditHelpers::CleanExportedValue(Override.Value);

		// Skip empty values
		if (Override.Value.IsEmpty() || Override.Value == TEXT("()") || Override.Value == TEXT("None"))
			continue;

		Props.Add(MoveTemp(Override));
	}

	return Props;
}

// ---------------------------------------------------------------------------
// Decorator extraction
// ---------------------------------------------------------------------------

static FBTDecoratorAuditData GatherDecoratorData(const UBTDecorator* Decorator)
{
	FBTDecoratorAuditData Data;
	Data.ClassName = GetShortNodeClassName(Decorator);
	Data.AbortMode = GetAbortModeString(Decorator->GetFlowAbortMode());
	Data.bInversed = Decorator->IsInversed();
	Data.Properties = GatherNodeProperties(Decorator);
	return Data;
}

// ---------------------------------------------------------------------------
// Service extraction
// ---------------------------------------------------------------------------

static FBTServiceAuditData GatherServiceData(const UBTService* Service)
{
	FBTServiceAuditData Data;
	Data.ClassName = GetShortNodeClassName(Service);

	// Interval and RandomDeviation are protected. Read via UPROPERTY reflection.
	if (const FFloatProperty* IntervalProp = CastField<FFloatProperty>(Service->GetClass()->FindPropertyByName(TEXT("Interval"))))
	{
		Data.Interval = IntervalProp->GetPropertyValue_InContainer(Service);
	}
	if (const FFloatProperty* DeviationProp = CastField<FFloatProperty>(Service->GetClass()->FindPropertyByName(TEXT("RandomDeviation"))))
	{
		Data.RandomDeviation = DeviationProp->GetPropertyValue_InContainer(Service);
	}

	Data.Properties = GatherNodeProperties(Service);
	return Data;
}

// ---------------------------------------------------------------------------
// Decorator logic (AND/OR/NOT)
// ---------------------------------------------------------------------------

static FString BuildDecoratorLogicString(
	const TArray<TObjectPtr<UBTDecorator>>& Decorators,
	const TArray<FBTDecoratorLogic>& DecoratorOps)
{
	if (Decorators.Num() <= 1 || DecoratorOps.Num() == 0)
	{
		return FString();
	}

	// Walk the ops to build an expression string
	// The ops array uses a prefix notation: And(2), Test, Test means "Test AND Test"
	FString Result;
	int32 DecIdx = 0;

	for (const FBTDecoratorLogic& Op : DecoratorOps)
	{
		switch (Op.Operation)
		{
		case EBTDecoratorLogic::Test:
			if (DecIdx < Decorators.Num() && Decorators[DecIdx])
			{
				if (!Result.IsEmpty()) Result += TEXT(" ");
				Result += FString::Printf(TEXT("(%s)"), *GetShortNodeClassName(Decorators[DecIdx]));
				DecIdx++;
			}
			break;
		case EBTDecoratorLogic::And:
			if (!Result.IsEmpty()) Result += TEXT(" ");
			Result += TEXT("AND");
			break;
		case EBTDecoratorLogic::Or:
			if (!Result.IsEmpty()) Result += TEXT(" ");
			Result += TEXT("OR");
			break;
		case EBTDecoratorLogic::Not:
			if (!Result.IsEmpty()) Result += TEXT(" ");
			Result += TEXT("NOT");
			break;
		default:
			break;
		}
	}

	return Result;
}

// ---------------------------------------------------------------------------
// Recursive tree walk
// ---------------------------------------------------------------------------

static FBTNodeAuditData GatherCompositeNodeData(const UBTCompositeNode* Composite);

static FBTNodeAuditData GatherTaskNodeData(const UBTTaskNode* Task)
{
	FBTNodeAuditData Data;
	Data.Type = TEXT("Task");
	Data.ClassName = GetShortNodeClassName(Task);
	Data.Properties = GatherNodeProperties(Task);

	// Task-level services
	for (const auto& Service : Task->Services)
	{
		if (Service)
			Data.Services.Add(GatherServiceData(Service));
	}

	return Data;
}

static FBTNodeAuditData GatherCompositeNodeData(const UBTCompositeNode* Composite)
{
	FBTNodeAuditData Data;
	Data.ClassName = GetShortNodeClassName(Composite);

	// Determine composite type
	if (const auto* SP = Cast<UBTComposite_SimpleParallel>(Composite))
	{
		Data.Type = TEXT("SimpleParallel");
		Data.FinishMode = SP->FinishMode == EBTParallelMode::AbortBackground
			? TEXT("AbortBackground")
			: TEXT("WaitForBackground");
	}
	else
	{
		// Use class name to distinguish Selector vs Sequence vs custom
		FString ClassName = Composite->GetClass()->GetName();
		if (ClassName.Contains(TEXT("Selector")))
			Data.Type = TEXT("Selector");
		else if (ClassName.Contains(TEXT("Sequence")))
			Data.Type = TEXT("Sequence");
		else
			Data.Type = ClassName;
	}

	// Composite-level services
	for (const auto& Service : Composite->Services)
	{
		if (Service)
			Data.Services.Add(GatherServiceData(Service));
	}

	// Children
	for (int32 i = 0; i < Composite->Children.Num(); ++i)
	{
		const FBTCompositeChild& Child = Composite->Children[i];

		FBTNodeAuditData ChildData;
		if (Child.ChildComposite)
		{
			ChildData = GatherCompositeNodeData(Child.ChildComposite);
		}
		else if (Child.ChildTask)
		{
			ChildData = GatherTaskNodeData(Child.ChildTask);
		}
		else
		{
			continue;
		}

		// Decorators on this child branch
		for (const auto& Dec : Child.Decorators)
		{
			if (Dec)
				ChildData.Decorators.Add(GatherDecoratorData(Dec));
		}

		// Decorator logic expression
		ChildData.DecoratorLogic = BuildDecoratorLogicString(Child.Decorators, Child.DecoratorOps);

		Data.Children.Add(MoveTemp(ChildData));
	}

	return Data;
}

// ---------------------------------------------------------------------------
// Blackboard extraction
// ---------------------------------------------------------------------------

static void GatherBlackboardKeys(const UBlackboardData* BBData, TArray<FBlackboardKeyAuditData>& OutKeys, bool bInherited)
{
	if (!BBData) return;

	// Walk parent chain first
	if (BBData->Parent)
	{
		GatherBlackboardKeys(BBData->Parent, OutKeys, true);
	}

	for (const FBlackboardEntry& Entry : BBData->Keys)
	{
		FBlackboardKeyAuditData KeyData;
		KeyData.Name = Entry.EntryName.ToString();
		KeyData.Type = GetBlackboardKeyTypeString(Entry.KeyType);
		KeyData.bInherited = bInherited;
		OutKeys.Add(MoveTemp(KeyData));
	}
}

// ---------------------------------------------------------------------------
// GatherData
// ---------------------------------------------------------------------------

FBehaviorTreeAuditData FBehaviorTreeAuditor::GatherData(const UBehaviorTree* BT)
{
	FBehaviorTreeAuditData Data;

	Data.Name = BT->GetName();
	Data.Path = BT->GetPathName();
	Data.PackageName = BT->GetOutermost()->GetName();
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	// Blackboard
	if (const UBlackboardData* BB = BT->BlackboardAsset)
	{
		Data.BlackboardAssetName = BB->GetName();
		Data.BlackboardAssetPath = BB->GetPathName();
		GatherBlackboardKeys(BB, Data.BlackboardKeys, false);
	}

	// Tree structure
	if (BT->RootNode)
	{
		Data.RootNode = GatherCompositeNodeData(BT->RootNode);

		// Root-level decorators (apply to entire tree, used by subtrees)
		for (const auto& Dec : BT->RootDecorators)
		{
			if (Dec)
				Data.RootNode.Decorators.Add(GatherDecoratorData(Dec));
		}
		Data.RootNode.DecoratorLogic = BuildDecoratorLogicString(BT->RootDecorators, BT->RootDecoratorOps);
	}

	return Data;
}

// ---------------------------------------------------------------------------
// SerializeToMarkdown - helpers
// ---------------------------------------------------------------------------

static void SerializePropertiesToMarkdown(FString& Result, const TArray<FPropertyOverrideData>& Properties, const FString& Indent)
{
	for (const auto& Prop : Properties)
	{
		Result += FString::Printf(TEXT("%s%s: %s\n"), *Indent, *Prop.Name, *Prop.Value);
	}
}

static void SerializeServicesToMarkdown(FString& Result, const TArray<FBTServiceAuditData>& Services, const FString& Indent)
{
	const FString ContentIndent = Indent + TEXT("  ");
	for (const auto& Svc : Services)
	{
		Result += FString::Printf(TEXT("%s- _service:_ %s (every %.1fs"),
			*Indent, *Svc.ClassName, Svc.Interval);
		if (Svc.RandomDeviation > 0.f)
			Result += FString::Printf(TEXT(" +/- %.1fs"), Svc.RandomDeviation);
		Result += TEXT(")\n");

		if (Svc.Properties.Num() > 0)
			SerializePropertiesToMarkdown(Result, Svc.Properties, ContentIndent);
	}
}

static void SerializeDecoratorsToMarkdown(FString& Result, const TArray<FBTDecoratorAuditData>& Decorators, const FString& Indent)
{
	const FString ContentIndent = Indent + TEXT("  ");
	for (const auto& Dec : Decorators)
	{
		FString DecStr = Indent + TEXT("- _decorator:_ ");
		if (Dec.bInversed) DecStr += TEXT("NOT ");
		DecStr += Dec.ClassName;

		if (Dec.AbortMode != TEXT("None") && !Dec.AbortMode.IsEmpty())
			DecStr += FString::Printf(TEXT(" (abort=%s)"), *Dec.AbortMode);

		Result += DecStr + TEXT("\n");

		if (Dec.Properties.Num() > 0)
			SerializePropertiesToMarkdown(Result, Dec.Properties, ContentIndent);
	}
}

static void SerializeNodeToMarkdown(FString& Result, const FBTNodeAuditData& Node, int32 Depth)
{
	const FString Indent = FString::ChrN(Depth * 2, TEXT(' '));

	if (Node.Type == TEXT("Task"))
	{
		// Task node (leaf)
		const FString ContentIndent = Indent + TEXT("  ");
		Result += FString::Printf(TEXT("%s- %s\n"), *Indent, *Node.ClassName);

		if (Node.Properties.Num() > 0)
			SerializePropertiesToMarkdown(Result, Node.Properties, ContentIndent);

		// Task-level services
		SerializeServicesToMarkdown(Result, Node.Services, Indent + TEXT("  "));
	}
	else
	{
		// Composite node
		FString Header = FString::Printf(TEXT("%s- **%s**"), *Indent, *Node.Type);
		if (!Node.FinishMode.IsEmpty())
			Header += FString::Printf(TEXT(" (FinishMode: %s)"), *Node.FinishMode);
		Result += Header + TEXT("\n");

		const FString ChildIndent = Indent + TEXT("  ");

		// Composite-level services
		SerializeServicesToMarkdown(Result, Node.Services, ChildIndent);

		// Children (decorators are on the children, not the composite)
		for (int32 i = 0; i < Node.Children.Num(); ++i)
		{
			const FBTNodeAuditData& Child = Node.Children[i];

			// Decorators on this branch
			SerializeDecoratorsToMarkdown(Result, Child.Decorators, ChildIndent);

			// SimpleParallel labels
			if (Node.Type == TEXT("SimpleParallel") && Node.Children.Num() == 2)
			{
				Result += FString::Printf(TEXT("%s[%s]\n"), *ChildIndent, i == 0 ? TEXT("main") : TEXT("background"));
			}

			SerializeNodeToMarkdown(Result, Child, Depth + 1);
		}
	}
}

// ---------------------------------------------------------------------------
// SerializeToMarkdown
// ---------------------------------------------------------------------------

FString FBehaviorTreeAuditor::SerializeToMarkdown(const FBehaviorTreeAuditData& Data)
{
	FString Result;
	Result.Reserve(4096);

	// Header
	Result += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Result += FString::Printf(TEXT("Path: %s\n"), *Data.Path);
	Result += TEXT("Type: BehaviorTree\n");

	if (!Data.SourceFilePath.IsEmpty())
	{
		Result += FString::Printf(TEXT("Hash: %s\n"), *FAuditFileUtils::ComputeFileHash(Data.SourceFilePath));
	}

	if (!Data.BlackboardAssetPath.IsEmpty())
	{
		Result += FString::Printf(TEXT("Blackboard: %s (%s)\n"), *Data.BlackboardAssetName, *Data.BlackboardAssetPath);
	}

	// Blackboard keys
	if (Data.BlackboardKeys.Num() > 0)
	{
		bool bHasInherited = false;
		bool bHasOwn = false;
		for (const auto& Key : Data.BlackboardKeys)
		{
			if (Key.bInherited) bHasInherited = true;
			else bHasOwn = true;
		}

		Result += TEXT("\n## Blackboard Keys\n");

		if (bHasInherited)
		{
			Result += FString::Printf(TEXT("\n### Inherited\n"));
			for (const auto& Key : Data.BlackboardKeys)
			{
				if (Key.bInherited)
					Result += FString::Printf(TEXT("- %s (%s)\n"), *Key.Name, *Key.Type);
			}
		}

		if (bHasOwn)
		{
			if (bHasInherited)
				Result += TEXT("\n### Own\n");
			for (const auto& Key : Data.BlackboardKeys)
			{
				if (!Key.bInherited)
					Result += FString::Printf(TEXT("- %s (%s)\n"), *Key.Name, *Key.Type);
			}
		}
	}

	// Tree
	if (!Data.RootNode.Type.IsEmpty())
	{
		Result += TEXT("\n## Tree\n\n");

		// Root-level decorators
		SerializeDecoratorsToMarkdown(Result, Data.RootNode.Decorators, TEXT(""));

		SerializeNodeToMarkdown(Result, Data.RootNode, 0);
	}

	return Result;
}

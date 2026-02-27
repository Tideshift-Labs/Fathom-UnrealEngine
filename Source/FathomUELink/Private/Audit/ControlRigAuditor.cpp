#include "Audit/ControlRigAuditor.h"

#include "Audit/AuditFileUtils.h"
#include "FathomUELinkModule.h"
#include "ControlRigBlueprint.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"

namespace
{
	FString RigVMPinDirectionToString(ERigVMPinDirection Dir)
	{
		switch (Dir)
		{
		case ERigVMPinDirection::Input:   return TEXT("Input");
		case ERigVMPinDirection::Output:  return TEXT("Output");
		case ERigVMPinDirection::IO:      return TEXT("IO");
		case ERigVMPinDirection::Hidden:  return TEXT("Hidden");
		default:                          return TEXT("Unknown");
		}
	}
}

FControlRigAuditData FControlRigAuditor::GatherData(const UControlRigBlueprint* CRBP)
{
	FControlRigAuditData Data;

	// --- Metadata ---
	Data.Name = CRBP->GetName();
	Data.Path = CRBP->GetPathName();
	Data.PackageName = CRBP->GetOutermost()->GetName();
	Data.ParentClass = CRBP->ParentClass ? CRBP->ParentClass->GetPathName() : TEXT("None");
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Gathering ControlRig data for %s"), *Data.Name);

	// --- Variables ---
	URigVMGraph* DefaultModel = CRBP->GetDefaultModel();
	if (DefaultModel)
	{
		TArray<FRigVMGraphVariableDescription> VarDescs = DefaultModel->GetVariableDescriptions();
		Data.Variables.Reserve(VarDescs.Num());
		for (const FRigVMGraphVariableDescription& Desc : VarDescs)
		{
			FVariableAuditData VarData;
			VarData.Name = Desc.Name.ToString();
			VarData.Type = Desc.CPPType;
			Data.Variables.Add(MoveTemp(VarData));
		}
	}

	// --- Graphs ---
	TArray<URigVMGraph*> AllModels = CRBP->GetAllModels();
	Data.Graphs.Reserve(AllModels.Num());

	for (URigVMGraph* Graph : AllModels)
	{
		if (!Graph) continue;

		FRigVMGraphAuditData GraphData;
		GraphData.Name = Graph->GetName();
		GraphData.bIsRootGraph = (Graph == DefaultModel);

		// Build node list, skipping reroute and comment nodes
		TMap<URigVMNode*, int32> NodeIdMap;
		int32 NextId = 0;

		for (URigVMNode* Node : Graph->GetNodes())
		{
			if (!Node) continue;
			if (Cast<URigVMRerouteNode>(Node) || Cast<URigVMCommentNode>(Node))
			{
				continue;
			}

			const int32 NodeId = NextId++;
			NodeIdMap.Add(Node, NodeId);

			FRigVMNodeAuditData NodeData;
			NodeData.Id = NodeId;
			NodeData.Name = Node->GetName();
			NodeData.bIsMutable = Node->IsMutable();
			NodeData.bIsPure = Node->IsPure();
			NodeData.bIsEvent = Node->IsEvent();

			// Classify by subclass
			if (const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
			{
				NodeData.Type = TEXT("Unit");
				if (UScriptStruct* Struct = UnitNode->GetScriptStruct())
				{
					NodeData.StructPath = Struct->GetPathName();
				}
				NodeData.MethodName = UnitNode->GetMethodName().ToString();
			}
			else if (Cast<URigVMVariableNode>(Node))
			{
				NodeData.Type = TEXT("Variable");
			}
			else if (Cast<URigVMFunctionReferenceNode>(Node))
			{
				NodeData.Type = TEXT("FunctionRef");
			}
			else if (Cast<URigVMFunctionEntryNode>(Node))
			{
				NodeData.Type = TEXT("FunctionEntry");

				// Extract input parameters from the entry node's pins
				for (URigVMPin* Pin : Node->GetPins())
				{
					if (Pin->GetDirection() == ERigVMPinDirection::Output && Pin->GetCPPType() != TEXT("FRigVMExecuteContext"))
					{
						FGraphParamData Param;
						Param.Name = Pin->GetName();
						Param.Type = Pin->GetCPPType();
						GraphData.Inputs.Add(MoveTemp(Param));
					}
				}
			}
			else if (Cast<URigVMFunctionReturnNode>(Node))
			{
				NodeData.Type = TEXT("FunctionReturn");

				// Extract output parameters from the return node's pins
				for (URigVMPin* Pin : Node->GetPins())
				{
					if (Pin->GetDirection() == ERigVMPinDirection::Input && Pin->GetCPPType() != TEXT("FRigVMExecuteContext"))
					{
						FGraphParamData Param;
						Param.Name = Pin->GetName();
						Param.Type = Pin->GetCPPType();
						GraphData.Outputs.Add(MoveTemp(Param));
					}
				}
			}
			else if (Cast<URigVMCollapseNode>(Node))
			{
				NodeData.Type = TEXT("Collapse");
			}
			else
			{
				NodeData.Type = TEXT("Other");
			}

			// Capture top-level pins
			for (URigVMPin* Pin : Node->GetPins())
			{
				FRigVMPinAuditData PinData;
				PinData.Name = Pin->GetName();
				PinData.CPPType = Pin->GetCPPType();
				PinData.Direction = RigVMPinDirectionToString(Pin->GetDirection());
				PinData.DefaultValue = Pin->GetDefaultValue();
				NodeData.Pins.Add(MoveTemp(PinData));
			}

			GraphData.Nodes.Add(MoveTemp(NodeData));
		}

		// --- Edges ---
		for (URigVMLink* Link : Graph->GetLinks())
		{
			if (!Link) continue;

			URigVMPin* SourcePin = Link->GetSourcePin();
			URigVMPin* TargetPin = Link->GetTargetPin();
			if (!SourcePin || !TargetPin) continue;

			URigVMNode* SourceNode = SourcePin->GetNode();
			URigVMNode* TargetNode = TargetPin->GetNode();
			if (!SourceNode || !TargetNode) continue;

			const int32* SourceIdPtr = NodeIdMap.Find(SourceNode);
			const int32* TargetIdPtr = NodeIdMap.Find(TargetNode);
			if (!SourceIdPtr || !TargetIdPtr) continue; // skipped nodes (reroute/comment)

			FRigVMEdgeAuditData Edge;
			Edge.SourceNodeId = *SourceIdPtr;
			Edge.SourcePinPath = SourcePin->GetPinPath();
			Edge.TargetNodeId = *TargetIdPtr;
			Edge.TargetPinPath = TargetPin->GetPinPath();
			GraphData.Edges.Add(MoveTemp(Edge));
		}

		Data.Graphs.Add(MoveTemp(GraphData));
	}

	return Data;
}

FString FControlRigAuditor::SerializeToMarkdown(const FControlRigAuditData& Data)
{
	FString Result;
	Result.Reserve(4096);

	// --- Header block ---
	Result += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Result += FString::Printf(TEXT("Path: %s\n"), *Data.Path);
	Result += FString::Printf(TEXT("Parent: %s\n"), *Data.ParentClass);
	Result += TEXT("Type: ControlRig\n");

	if (!Data.SourceFilePath.IsEmpty())
	{
		Result += FString::Printf(TEXT("Hash: %s\n"), *FAuditFileUtils::ComputeFileHash(Data.SourceFilePath));
	}

	// --- Variables ---
	if (Data.Variables.Num() > 0)
	{
		Result += TEXT("\n## Variables\n");
		Result += TEXT("| Name | Type |\n");
		Result += TEXT("|------|------|\n");
		for (const FVariableAuditData& Var : Data.Variables)
		{
			Result += FString::Printf(TEXT("| %s | %s |\n"), *Var.Name, *Var.Type);
		}
	}

	// --- Graphs ---
	for (const FRigVMGraphAuditData& Graph : Data.Graphs)
	{
		Result += FString::Printf(TEXT("\n## Graph: %s\n"), *Graph.Name);

		if (Graph.bIsRootGraph)
		{
			Result += TEXT("(root graph)\n");
		}

		// Signature (inputs/outputs)
		if (Graph.Inputs.Num() > 0)
		{
			Result += TEXT("\n### Inputs\n");
			for (const FGraphParamData& Param : Graph.Inputs)
			{
				Result += FString::Printf(TEXT("- %s (%s)\n"), *Param.Name, *Param.Type);
			}
		}

		if (Graph.Outputs.Num() > 0)
		{
			Result += TEXT("\n### Outputs\n");
			for (const FGraphParamData& Param : Graph.Outputs)
			{
				Result += FString::Printf(TEXT("- %s (%s)\n"), *Param.Name, *Param.Type);
			}
		}

		// Nodes
		if (Graph.Nodes.Num() > 0)
		{
			Result += TEXT("\n### Nodes\n");
			Result += TEXT("| Id | Type | Name | Details |\n");
			Result += TEXT("|----|------|------|---------|\n");
			for (const FRigVMNodeAuditData& Node : Graph.Nodes)
			{
				FString Details;
				if (!Node.StructPath.IsEmpty())
				{
					Details += Node.StructPath;
				}
				if (Node.bIsEvent)
				{
					if (!Details.IsEmpty()) Details += TEXT(", ");
					Details += TEXT("event");
				}
				if (Node.bIsMutable)
				{
					if (!Details.IsEmpty()) Details += TEXT(", ");
					Details += TEXT("mutable");
				}
				if (Node.bIsPure)
				{
					if (!Details.IsEmpty()) Details += TEXT(", ");
					Details += TEXT("pure");
				}

				Result += FString::Printf(TEXT("| %d | %s | %s | %s |\n"),
					Node.Id, *Node.Type, *Node.Name, *Details);
			}
		}

		// Edges
		if (Graph.Edges.Num() > 0)
		{
			Result += TEXT("\n### Edges\n");
			for (const FRigVMEdgeAuditData& Edge : Graph.Edges)
			{
				Result += FString::Printf(TEXT("%d.%s -> %d.%s\n"),
					Edge.SourceNodeId, *Edge.SourcePinPath,
					Edge.TargetNodeId, *Edge.TargetPinPath);
			}
		}
	}

	return Result;
}

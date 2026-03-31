#include "Audit/MaterialAuditor.h"

#include "Audit/AuditFileUtils.h"
#include "FathomUELinkModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Audit/AuditHelpers.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "StaticParameterSet.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Collect all editable, non-transient properties from the given object. */
	void GatherEditableProperties(const UObject* Object, const UClass* StopAtClass, TArray<FPropertyOverrideData>& OutProperties)
	{
		for (TFieldIterator<FProperty> PropIt(Object->GetClass()); PropIt; ++PropIt)
		{
			const FProperty* Prop = *PropIt;

			// Stop at the base class (don't include UObject/UMaterialInterface internals)
			if (StopAtClass && Prop->GetOwner<UClass>() == StopAtClass)
			{
				continue;
			}

			// Only include properties visible in the Details panel
			if (!Prop->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}

			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			FString Value;
			Prop->ExportText_InContainer(0, Value, Object, nullptr, nullptr, 0);

			// Skip empty values
			if (Value.IsEmpty())
			{
				continue;
			}

			FPropertyOverrideData Entry;
			Entry.Name = Prop->GetName();
			Entry.Value = FathomAuditHelpers::CleanExportedValue(Value);
			OutProperties.Add(MoveTemp(Entry));
		}
	}
}

FMaterialAuditData FMaterialAuditor::GatherData(const UMaterialInterface* Material)
{
	FMaterialAuditData Data;

	Data.Name = Material->GetName();
	Data.Path = Material->GetPathName();
	Data.PackageName = Material->GetOutermost()->GetName();
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	// Core properties via UMaterialInterface virtual methods
	Data.BlendMode = StaticEnum<EBlendMode>()->GetNameStringByValue((int64)Material->GetBlendMode());
	Data.bTwoSided = Material->IsTwoSided();

	// Shading model(s)
	{
		FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
		TArray<FString> ModelNames;
		for (int32 i = 0; i < (int32)EMaterialShadingModel::MSM_NUM; ++i)
		{
			EMaterialShadingModel Model = (EMaterialShadingModel)i;
			if (ShadingModels.HasShadingModel(Model))
			{
				ModelNames.Add(StaticEnum<EMaterialShadingModel>()->GetNameStringByValue((int64)Model));
			}
		}
		Data.ShadingModel = FString::Join(ModelNames, TEXT(", "));
	}

	// Check if this is a material instance
	if (const UMaterialInstance* MI = Cast<UMaterialInstance>(Material))
	{
		Data.bIsMaterialInstance = true;
		if (MI->Parent)
		{
			Data.ParentPath = MI->Parent->GetPathName();
		}

		// Material domain from the base material
		if (const UMaterial* BaseMat = const_cast<UMaterialInterface*>(Material)->GetBaseMaterial())
		{
			Data.MaterialDomain = StaticEnum<EMaterialDomain>()->GetNameStringByValue((int64)BaseMat->MaterialDomain);
		}

		// Scalar parameters
		for (const FScalarParameterValue& Param : MI->ScalarParameterValues)
		{
			FMaterialParameterData ParamData;
			ParamData.Name = Param.ParameterInfo.Name.ToString();
			ParamData.Value = FString::Printf(TEXT("%.4f"), Param.ParameterValue);
			Data.ScalarParameters.Add(MoveTemp(ParamData));
		}

		// Vector parameters
		for (const FVectorParameterValue& Param : MI->VectorParameterValues)
		{
			FMaterialParameterData ParamData;
			ParamData.Name = Param.ParameterInfo.Name.ToString();
			const FLinearColor& C = Param.ParameterValue;
			ParamData.Value = FString::Printf(TEXT("(%.2f, %.2f, %.2f, %.2f)"), C.R, C.G, C.B, C.A);
			Data.VectorParameters.Add(MoveTemp(ParamData));
		}

		// Texture parameters
		for (const FTextureParameterValue& Param : MI->TextureParameterValues)
		{
			FMaterialParameterData ParamData;
			ParamData.Name = Param.ParameterInfo.Name.ToString();
			ParamData.Value = Param.ParameterValue ? Param.ParameterValue->GetName() : TEXT("None");
			Data.TextureParameters.Add(MoveTemp(ParamData));
		}

		// Static switch parameters
		FStaticParameterSet StaticParams = MI->GetStaticParameters();
		for (const FStaticSwitchParameter& Param : StaticParams.StaticSwitchParameters)
		{
			FMaterialParameterData ParamData;
			ParamData.Name = Param.ParameterInfo.Name.ToString();
			ParamData.Value = Param.Value ? TEXT("true") : TEXT("false");
			Data.StaticSwitchParameters.Add(MoveTemp(ParamData));
		}

		// All editable properties
		GatherEditableProperties(MI, UMaterialInterface::StaticClass(), Data.Properties);
	}
	else if (const UMaterial* Mat = Cast<UMaterial>(Material))
	{
		// Base material properties
		Data.MaterialDomain = StaticEnum<EMaterialDomain>()->GetNameStringByValue((int64)Mat->MaterialDomain);

		// Expression stats
		const auto& Expressions = Mat->GetExpressions();
		Data.ExpressionCount = Expressions.Num();

		int32 TexSampleCount = 0;
		for (const auto& Expr : Expressions)
		{
			if (Cast<UMaterialExpressionTextureBase>(Expr))
			{
				++TexSampleCount;
			}
		}
		Data.TextureSampleCount = TexSampleCount;

		// Expression graph: nodes, edges, and material output connections
		{
			TMap<const UMaterialExpression*, int32> ExprIdMap;
			int32 NextId = 0;

			// Pass 1: Build node list
			for (const auto& Expr : Expressions)
			{
				if (!Expr) continue;

				const int32 Id = NextId++;
				ExprIdMap.Add(Expr, Id);

				FMaterialExpressionData ExprData;
				ExprData.Id = Id;

				// Type: strip "MaterialExpression" prefix from class name
				FString ClassName = Expr->GetClass()->GetName();
				ClassName.RemoveFromStart(TEXT("MaterialExpression"));
				ExprData.Type = ClassName;

				// Name: prefer parameter name, fall back to caption
				if (Expr->HasAParameterName())
				{
					ExprData.Name = Expr->GetParameterName().ToString();
				}
				else
				{
					TArray<FString> Captions;
					Expr->GetCaption(Captions);
					if (Captions.Num() > 0)
					{
						ExprData.Name = Captions[0];
					}
				}

				// Default values for unconnected input pins
				for (int32 InputIdx = 0; ; ++InputIdx)
				{
					const FExpressionInput* Input = Expr->GetInput(InputIdx);
					if (!Input) break;
					if (Input->Expression) continue; // connected, skip

					FString DefaultVal = const_cast<UMaterialExpression*>(Expr.Get())->GetInputPinDefaultValue(InputIdx);
					if (!DefaultVal.IsEmpty())
					{
						FDefaultInputData DefaultData;
						DefaultData.Name = Expr->GetInputName(InputIdx).ToString();
						DefaultData.Value = MoveTemp(DefaultVal);
						ExprData.DefaultInputs.Add(MoveTemp(DefaultData));
					}
				}

				Data.Expressions.Add(MoveTemp(ExprData));
			}

			// Pass 2: Build edges by walking inputs of each expression
			for (const auto& Expr : Expressions)
			{
				if (!Expr) continue;

				const int32* TargetIdPtr = ExprIdMap.Find(Expr);
				if (!TargetIdPtr) continue;

				for (FExpressionInputIterator It(const_cast<UMaterialExpression*>(Expr.Get())); It; ++It)
				{
					FExpressionInput* Input = It.Input;
					if (!Input || !Input->Expression) continue;

					const int32* SourceIdPtr = ExprIdMap.Find(Input->Expression);
					if (!SourceIdPtr) continue;

					FMaterialEdgeData Edge;
					Edge.SourceNodeId = *SourceIdPtr;
					Edge.TargetNodeId = *TargetIdPtr;
					Edge.TargetInput = Expr->GetInputName(It.Index).ToString();

					// Source output name
					const TArray<FExpressionOutput>& Outputs = Input->Expression->Outputs;
					if (Outputs.IsValidIndex(Input->OutputIndex) && !Outputs[Input->OutputIndex].OutputName.IsNone())
					{
						Edge.SourceOutput = Outputs[Input->OutputIndex].OutputName.ToString();
					}

					Data.Edges.Add(MoveTemp(Edge));
				}
			}

			// Pass 3: Material output pin connections (BaseColor, Metallic, etc.)
			if (const UMaterialEditorOnlyData* EditorData = Mat->GetEditorOnlyData())
			{
				auto AddOutputConnection = [&](const TCHAR* PinName, const FExpressionInput& Input)
				{
					if (Input.Expression)
					{
						const int32* SrcId = ExprIdMap.Find(Input.Expression);
						if (SrcId)
						{
							FMaterialOutputConnection Conn;
							Conn.OutputName = PinName;
							Conn.SourceNodeId = *SrcId;
							const TArray<FExpressionOutput>& Outputs = Input.Expression->Outputs;
							if (Outputs.IsValidIndex(Input.OutputIndex) && !Outputs[Input.OutputIndex].OutputName.IsNone())
							{
								Conn.SourceOutput = Outputs[Input.OutputIndex].OutputName.ToString();
							}
							Data.OutputConnections.Add(MoveTemp(Conn));
						}
					}
				};

				AddOutputConnection(TEXT("BaseColor"), EditorData->BaseColor);
				AddOutputConnection(TEXT("Metallic"), EditorData->Metallic);
				AddOutputConnection(TEXT("Specular"), EditorData->Specular);
				AddOutputConnection(TEXT("Roughness"), EditorData->Roughness);
				AddOutputConnection(TEXT("Anisotropy"), EditorData->Anisotropy);
				AddOutputConnection(TEXT("Normal"), EditorData->Normal);
				AddOutputConnection(TEXT("Tangent"), EditorData->Tangent);
				AddOutputConnection(TEXT("EmissiveColor"), EditorData->EmissiveColor);
				AddOutputConnection(TEXT("Opacity"), EditorData->Opacity);
				AddOutputConnection(TEXT("OpacityMask"), EditorData->OpacityMask);
				AddOutputConnection(TEXT("WorldPositionOffset"), EditorData->WorldPositionOffset);
				AddOutputConnection(TEXT("SubsurfaceColor"), EditorData->SubsurfaceColor);
				AddOutputConnection(TEXT("ClearCoat"), EditorData->ClearCoat);
				AddOutputConnection(TEXT("ClearCoatRoughness"), EditorData->ClearCoatRoughness);
				AddOutputConnection(TEXT("AmbientOcclusion"), EditorData->AmbientOcclusion);
				AddOutputConnection(TEXT("Refraction"), EditorData->Refraction);
				AddOutputConnection(TEXT("PixelDepthOffset"), EditorData->PixelDepthOffset);
			}
		}

		// Enumerate parameters via UMaterialInterface API
		TArray<FMaterialParameterInfo> ParamInfos;
		TArray<FGuid> ParamIds;

		// Scalar
		Material->GetAllScalarParameterInfo(ParamInfos, ParamIds);
		for (const FMaterialParameterInfo& Info : ParamInfos)
		{
			float Value = 0.f;
			Material->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Info), Value);
			FMaterialParameterData ParamData;
			ParamData.Name = Info.Name.ToString();
			ParamData.Value = FString::Printf(TEXT("%.4f"), Value);
			Data.ScalarParameters.Add(MoveTemp(ParamData));
		}

		// Vector
		ParamInfos.Reset();
		ParamIds.Reset();
		Material->GetAllVectorParameterInfo(ParamInfos, ParamIds);
		for (const FMaterialParameterInfo& Info : ParamInfos)
		{
			FLinearColor Value = FLinearColor::Black;
			Material->GetVectorParameterDefaultValue(FHashedMaterialParameterInfo(Info), Value);
			FMaterialParameterData ParamData;
			ParamData.Name = Info.Name.ToString();
			ParamData.Value = FString::Printf(TEXT("(%.2f, %.2f, %.2f, %.2f)"), Value.R, Value.G, Value.B, Value.A);
			Data.VectorParameters.Add(MoveTemp(ParamData));
		}

		// Texture
		ParamInfos.Reset();
		ParamIds.Reset();
		Material->GetAllTextureParameterInfo(ParamInfos, ParamIds);
		for (const FMaterialParameterInfo& Info : ParamInfos)
		{
			UTexture* Value = nullptr;
			Material->GetTextureParameterDefaultValue(FHashedMaterialParameterInfo(Info), Value);
			FMaterialParameterData ParamData;
			ParamData.Name = Info.Name.ToString();
			ParamData.Value = Value ? Value->GetName() : TEXT("None");
			Data.TextureParameters.Add(MoveTemp(ParamData));
		}

		// Static switches
		ParamInfos.Reset();
		ParamIds.Reset();
		Material->GetAllStaticSwitchParameterInfo(ParamInfos, ParamIds);
		for (const FMaterialParameterInfo& Info : ParamInfos)
		{
			FMaterialParameterData ParamData;
			ParamData.Name = Info.Name.ToString();
			// Base materials don't have static switch values (only instances override them)
			ParamData.Value = TEXT("(default)");
			Data.StaticSwitchParameters.Add(MoveTemp(ParamData));
		}

		// All editable properties
		GatherEditableProperties(Mat, UMaterialInterface::StaticClass(), Data.Properties);
	}

	return Data;
}

FString FMaterialAuditor::SerializeToMarkdown(const FMaterialAuditData& Data)
{
	FString Result;
	Result.Reserve(2048);

	// Header
	Result += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Result += FString::Printf(TEXT("Path: %s\n"), *Data.Path);

	if (Data.bIsMaterialInstance)
	{
		Result += TEXT("Instance: Yes\n");
		if (!Data.ParentPath.IsEmpty())
		{
			Result += FString::Printf(TEXT("Parent: %s\n"), *Data.ParentPath);
		}
	}

	if (!Data.MaterialDomain.IsEmpty())
	{
		Result += FString::Printf(TEXT("Domain: %s\n"), *Data.MaterialDomain);
	}
	Result += FString::Printf(TEXT("BlendMode: %s\n"), *Data.BlendMode);
	if (!Data.ShadingModel.IsEmpty())
	{
		Result += FString::Printf(TEXT("ShadingModel: %s\n"), *Data.ShadingModel);
	}
	Result += FString::Printf(TEXT("TwoSided: %s\n"), Data.bTwoSided ? TEXT("Yes") : TEXT("No"));

	if (!Data.bIsMaterialInstance)
	{
		Result += FString::Printf(TEXT("Expressions: %d\n"), Data.ExpressionCount);
		Result += FString::Printf(TEXT("TextureSamples: %d\n"), Data.TextureSampleCount);
	}

	if (!Data.SourceFilePath.IsEmpty())
	{
		Result += FString::Printf(TEXT("Hash: %s\n"), *FAuditFileUtils::ComputeFileHash(Data.SourceFilePath));
	}

	// Properties (all editable Details panel properties)
	if (Data.Properties.Num() > 0)
	{
		Result += TEXT("\n## Properties\n");
		for (const FPropertyOverrideData& Prop : Data.Properties)
		{
			Result += FString::Printf(TEXT("- %s = %s\n"), *Prop.Name, *Prop.Value);
		}
	}

	// Scalar Parameters
	if (Data.ScalarParameters.Num() > 0)
	{
		Result += TEXT("\n## Scalar Parameters\n");
		Result += TEXT("| Name | Value |\n");
		Result += TEXT("|------|-------|\n");
		for (const FMaterialParameterData& Param : Data.ScalarParameters)
		{
			Result += FString::Printf(TEXT("| %s | %s |\n"), *Param.Name, *Param.Value);
		}
	}

	// Vector Parameters
	if (Data.VectorParameters.Num() > 0)
	{
		Result += TEXT("\n## Vector Parameters\n");
		Result += TEXT("| Name | Value |\n");
		Result += TEXT("|------|-------|\n");
		for (const FMaterialParameterData& Param : Data.VectorParameters)
		{
			Result += FString::Printf(TEXT("| %s | %s |\n"), *Param.Name, *Param.Value);
		}
	}

	// Texture Parameters
	if (Data.TextureParameters.Num() > 0)
	{
		Result += TEXT("\n## Texture Parameters\n");
		Result += TEXT("| Name | Value |\n");
		Result += TEXT("|------|-------|\n");
		for (const FMaterialParameterData& Param : Data.TextureParameters)
		{
			Result += FString::Printf(TEXT("| %s | %s |\n"), *Param.Name, *Param.Value);
		}
	}

	// Static Switch Parameters
	if (Data.StaticSwitchParameters.Num() > 0)
	{
		Result += TEXT("\n## Static Switch Parameters\n");
		Result += TEXT("| Name | Value |\n");
		Result += TEXT("|------|-------|\n");
		for (const FMaterialParameterData& Param : Data.StaticSwitchParameters)
		{
			Result += FString::Printf(TEXT("| %s | %s |\n"), *Param.Name, *Param.Value);
		}
	}

	// Material output connections
	if (Data.OutputConnections.Num() > 0)
	{
		Result += TEXT("\n## Output Connections\n");
		for (const FMaterialOutputConnection& Conn : Data.OutputConnections)
		{
			// Look up the source node name
			FString NodeLabel = FString::Printf(TEXT("%d"), Conn.SourceNodeId);
			if (Data.Expressions.IsValidIndex(Conn.SourceNodeId) && !Data.Expressions[Conn.SourceNodeId].Name.IsEmpty())
			{
				NodeLabel = FString::Printf(TEXT("%s (%d)"), *Data.Expressions[Conn.SourceNodeId].Name, Conn.SourceNodeId);
			}

			if (Conn.SourceOutput.IsEmpty())
			{
				Result += FString::Printf(TEXT("- %s <- %s\n"), *Conn.OutputName, *NodeLabel);
			}
			else
			{
				Result += FString::Printf(TEXT("- %s <- %s.%s\n"), *Conn.OutputName, *NodeLabel, *Conn.SourceOutput);
			}
		}
	}

	// Expression graph
	if (Data.Expressions.Num() > 0)
	{
		Result += TEXT("\n## Expression Graph\n");

		Result += TEXT("\n### Nodes\n");
		Result += TEXT("| Id | Type | Name | Details |\n");
		Result += TEXT("|----|------|------|---------|\n");
		for (const FMaterialExpressionData& Expr : Data.Expressions)
		{
			FString Details;
			for (const FDefaultInputData& Default : Expr.DefaultInputs)
			{
				if (!Details.IsEmpty()) Details += TEXT(", ");
				Details += FString::Printf(TEXT("%s=%s"), *Default.Name, *Default.Value);
			}
			Result += FString::Printf(TEXT("| %d | %s | %s | %s |\n"), Expr.Id, *Expr.Type, *Expr.Name, *Details);
		}

		if (Data.Edges.Num() > 0)
		{
			Result += TEXT("\n### Edges\n");
			for (const FMaterialEdgeData& Edge : Data.Edges)
			{
				Result += TEXT("- ");
				Result += FString::Printf(TEXT("%d"), Edge.SourceNodeId);
				if (!Edge.SourceOutput.IsEmpty())
				{
					Result += FString::Printf(TEXT(".%s"), *Edge.SourceOutput);
				}
				Result += FString::Printf(TEXT(" -> %d"), Edge.TargetNodeId);
				if (!Edge.TargetInput.IsEmpty())
				{
					Result += FString::Printf(TEXT(".%s"), *Edge.TargetInput);
				}
				Result += TEXT("\n");
			}
		}
	}

	return Result;
}

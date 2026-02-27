#include "Audit/DataAssetAuditor.h"

#include "Audit/AuditHelpers.h"
#include "Audit/AuditFileUtils.h"
#include "Engine/DataAsset.h"
#include "UObject/UnrealType.h"

FDataAssetAuditData FDataAssetAuditor::GatherData(const UDataAsset* Asset)
{
	FDataAssetAuditData Data;

	Data.Name = Asset->GetName();
	Data.Path = Asset->GetPathName();
	Data.PackageName = Asset->GetOutermost()->GetName();
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	const UClass* AssetClass = Asset->GetClass();
	Data.NativeClass = AssetClass->GetName();
	Data.NativeClassPath = AssetClass->GetPathName();

	// CDO diff: compare asset properties against the class default object.
	// If the asset's class comes from a Blueprint with compile errors, the
	// CDO may not exist or may be in a broken state, so guard against that.
	const UObject* CDO = AssetClass->GetDefaultObject(/*bCreateIfNeeded=*/false);

	if (CDO)
	{
		for (TFieldIterator<FProperty> PropIt(AssetClass); PropIt; ++PropIt)
		{
			const FProperty* Prop = *PropIt;

			// Skip properties owned by the engine base class (UDataAsset itself)
			if (Prop->GetOwner<UClass>() == UDataAsset::StaticClass())
			{
				continue;
			}

			if (Prop->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}

			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Asset);
			const void* DefaultPtr = Prop->ContainerPtrToValuePtr<void>(CDO);

			if (!Prop->Identical(ValuePtr, DefaultPtr))
			{
				FPropertyOverrideData Override;
				Override.Name = Prop->GetName();
				Prop->ExportText_InContainer(0, Override.Value, Asset, nullptr, nullptr, 0);
				Data.Properties.Add(MoveTemp(Override));
			}
		}
	}

	return Data;
}

FString FDataAssetAuditor::SerializeToMarkdown(const FDataAssetAuditData& Data)
{
	FString Result;
	Result.Reserve(2048);

	// Header
	Result += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Result += FString::Printf(TEXT("Path: %s\n"), *Data.Path);
	Result += FString::Printf(TEXT("Class: %s\n"), *Data.NativeClass);
	if (!Data.NativeClassPath.IsEmpty())
	{
		Result += FString::Printf(TEXT("ClassPath: %s\n"), *Data.NativeClassPath);
	}

	if (!Data.SourceFilePath.IsEmpty())
	{
		Result += FString::Printf(TEXT("Hash: %s\n"), *FAuditFileUtils::ComputeFileHash(Data.SourceFilePath));
	}

	// Properties
	if (Data.Properties.Num() > 0)
	{
		Result += TEXT("\n## Properties\n");
		for (const FPropertyOverrideData& Prop : Data.Properties)
		{
			Result += FString::Printf(TEXT("- %s = %s\n"), *Prop.Name, *FathomAuditHelpers::CleanExportedValue(Prop.Value));
		}
	}

	return Result;
}

#include "Audit/AuditFileUtils.h"

#include "FathomUELinkModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/TopLevelAssetPath.h"

FString FAuditFileUtils::GetVariableTypeString(const FEdGraphPinType& PinType)
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

FString FAuditFileUtils::GetAuditOutputPath(const UBlueprint* BP)
{
	return GetAuditOutputPath(BP->GetOutermost()->GetName());
}

FString FAuditFileUtils::GetAuditBaseDir()
{
	const FString VersionDir = FString::Printf(TEXT("v%d"), AuditSchemaVersion);
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Saved") / TEXT("Fathom") / TEXT("Audit") / VersionDir);
}

FString FAuditFileUtils::GetAuditOutputPath(const FString& PackageName)
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

bool FAuditFileUtils::DeleteAuditFile(const FString& FilePath)
{
	IFileManager& FM = IFileManager::Get();
	if (!FM.FileExists(*FilePath))
	{
		return true;
	}

	if (FM.Delete(*FilePath))
	{
		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Deleted audit file %s"), *FilePath);
		return true;
	}

	UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to delete audit file %s"), *FilePath);
	return false;
}

FString FAuditFileUtils::GetSourceFilePath(const FString& PackageName)
{
	FString FilePath;
	if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, FilePath, FPackageName::GetAssetPackageExtension()))
	{
		return FPaths::ConvertRelativePathToFull(FilePath);
	}
	UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to resolve source path for %s"), *PackageName);
	return FString();
}

FString FAuditFileUtils::ComputeFileHash(const FString& FilePath)
{
	const FMD5Hash Hash = FMD5Hash::HashFile(*FilePath);
	if (Hash.IsValid())
	{
		return LexToString(Hash);
	}
	UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to compute hash for %s"), *FilePath);
	return FString();
}

bool FAuditFileUtils::WriteAuditFile(const FString& Content, const FString& OutputPath)
{
	if (FFileHelper::SaveStringToFile(Content, *OutputPath))
	{
		UE_LOG(LogFathomUELink, Verbose, TEXT("Fathom: Audit saved to %s"), *OutputPath);
		return true;
	}

	UE_LOG(LogFathomUELink, Error, TEXT("Fathom: Failed to write %s"), *OutputPath);
	return false;
}

void FAuditFileUtils::WriteAuditManifest()
{
	const FString VersionDir = FString::Printf(TEXT("v%d"), AuditSchemaVersion);
	const FString AuditDir = FString::Printf(TEXT("Saved/Fathom/Audit/%s"), *VersionDir);

	const FString Json = FString::Printf(
		TEXT("{\n  \"version\": %d,\n  \"auditDir\": \"%s\"\n}\n"),
		AuditSchemaVersion, *AuditDir);

	const FString ManifestPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Saved") / TEXT("Fathom") / TEXT("audit-manifest.json"));

	if (FFileHelper::SaveStringToFile(Json, *ManifestPath))
	{
		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Wrote audit manifest to %s"), *ManifestPath);
	}
	else
	{
		UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: Failed to write audit manifest to %s"), *ManifestPath);
	}
}

bool FAuditFileUtils::IsSupportedBlueprintClass(const FTopLevelAssetPath& ClassPath)
{
	// Previously excluded ControlRig/RigVM because LoadObject triggered fatal
	// assertions. With ControlRig and RigVMDeveloper modules now linked as
	// dependencies, the subsystems are initialized and loading should be safe.
	return true;
}

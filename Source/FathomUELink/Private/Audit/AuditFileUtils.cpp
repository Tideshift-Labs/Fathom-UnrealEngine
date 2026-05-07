#include "Audit/AuditFileUtils.h"

#include "FathomUELinkModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/CriticalSection.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	const FString GamePackageRoot = TEXT("/Game/");
	const FString PluginsAuditPrefix = TEXT("_Plugins/");

	FString ExtractMountName(const FString& PackageName)
	{
		// Package paths are /MountName/... ; pull out MountName.
		if (!PackageName.StartsWith(TEXT("/")))
		{
			return FString();
		}
		const int32 NextSlash = PackageName.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
		if (NextSlash <= 1)
		{
			return PackageName.Mid(1);
		}
		return PackageName.Mid(1, NextSlash - 1);
	}

	bool IsProjectPluginMountName(const FString& MountName)
	{
		// Cache results per mount name. Plugin enable/disable mid-session is rare;
		// the editor restart that follows will rebuild the cache.
		static FCriticalSection CacheLock;
		static TMap<FString, bool> Cache;

		{
			FScopeLock Lock(&CacheLock);
			if (const bool* Cached = Cache.Find(MountName))
			{
				return *Cached;
			}
		}

		bool bIsProjectPlugin = false;
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(MountName))
		{
			bIsProjectPlugin = Plugin->GetType() == EPluginType::Project;
		}

		FScopeLock Lock(&CacheLock);
		Cache.Add(MountName, bIsProjectPlugin);
		return bIsProjectPlugin;
	}
}

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
	// /Game/Foo/Bar      -> <base>/Foo/Bar.md
	// /MyPlugin/Foo/Bar  -> <base>/_Plugins/MyPlugin/Foo/Bar.md
	if (PackageName.StartsWith(GamePackageRoot))
	{
		FString RelativePath = PackageName;
		RelativePath.RightChopInline(GamePackageRoot.Len());
		return GetAuditBaseDir() / RelativePath + TEXT(".md");
	}

	const FString MountName = ExtractMountName(PackageName);
	if (!MountName.IsEmpty())
	{
		const FString MountPrefix = FString::Printf(TEXT("/%s/"), *MountName);
		FString RelativePath = PackageName;
		if (RelativePath.StartsWith(MountPrefix))
		{
			RelativePath.RightChopInline(MountPrefix.Len());
		}
		return GetAuditBaseDir() / PluginsAuditPrefix + MountName / RelativePath + TEXT(".md");
	}

	// Fallback: drop the leading slash and write under base.
	FString Fallback = PackageName;
	Fallback.RemoveFromStart(TEXT("/"));
	return GetAuditBaseDir() / Fallback + TEXT(".md");
}

bool FAuditFileUtils::IsAuditablePackage(const FString& PackageName)
{
	// Exclude One-File-Per-Actor / external-object packages from any content root.
	// These are level data (placed actors and their components), not first-class
	// assets we want to audit, and they're often missing from disk during edits
	// which produces noisy hash-failure warnings.
	if (PackageName.Contains(FPackagePath::GetExternalActorsFolderName()) ||
		PackageName.Contains(FPackagePath::GetExternalObjectsFolderName()))
	{
		return false;
	}

	if (PackageName.StartsWith(GamePackageRoot))
	{
		return true;
	}

	const FString MountName = ExtractMountName(PackageName);
	if (MountName.IsEmpty() || MountName == TEXT("Game"))
	{
		return false;
	}

	return IsProjectPluginMountName(MountName);
}

FString FAuditFileUtils::PackageNameFromRelativeAuditPath(const FString& RelPath)
{
	if (RelPath.IsEmpty())
	{
		return FString();
	}

	if (RelPath.StartsWith(PluginsAuditPrefix))
	{
		FString Remainder = RelPath;
		Remainder.RightChopInline(PluginsAuditPrefix.Len());
		// Remainder is now <PluginName>/<rest>
		int32 Slash = INDEX_NONE;
		if (!Remainder.FindChar(TEXT('/'), Slash) || Slash <= 0)
		{
			return FString();
		}
		const FString PluginName = Remainder.Left(Slash);
		const FString Rest = Remainder.Mid(Slash + 1);
		return FString::Printf(TEXT("/%s/%s"), *PluginName, *Rest);
	}

	return TEXT("/Game/") + RelPath;
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

FString FAuditFileUtils::ToProjectRelativeSourcePath(const FString& AbsPath)
{
	if (AbsPath.IsEmpty())
	{
		return FString();
	}

	FString Normalized = AbsPath;
	FPaths::NormalizeFilename(Normalized);

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);
	if (!ProjectDir.EndsWith(TEXT("/")))
	{
		ProjectDir += TEXT("/");
	}

	if (Normalized.StartsWith(ProjectDir, ESearchCase::IgnoreCase))
	{
		return Normalized.RightChop(ProjectDir.Len());
	}

	return Normalized;
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

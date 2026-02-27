#pragma once

#include "CoreMinimal.h"

class UBlueprint;
struct FEdGraphPinType;
struct FTopLevelAssetPath;

/**
 * Cross-cutting file and path utilities for the audit system.
 */
struct FATHOMUELINK_API FAuditFileUtils
{
	/** Bump when the audit format changes to invalidate all cached audit files. */
	static constexpr int32 AuditSchemaVersion = 10;

	/** Human-readable type string for a Blueprint variable pin type. */
	static FString GetVariableTypeString(const FEdGraphPinType& PinType);

	/** Return the base directory for all audit files: <ProjectDir>/Saved/Fathom/Audit/v<N>/ */
	static FString GetAuditBaseDir();

	/**
	 * Compute the on-disk output path for an asset's audit file.
	 * e.g. /Game/UI/Widgets/WBP_Foo  ->  <ProjectDir>/Saved/Fathom/Audit/v<N>/UI/Widgets/WBP_Foo.md
	 */
	static FString GetAuditOutputPath(const UBlueprint* BP);
	static FString GetAuditOutputPath(const FString& PackageName);

	/** Delete an audit file. Returns true if the file was deleted or did not exist. */
	static bool DeleteAuditFile(const FString& FilePath);

	/** Convert a package name (e.g. /Game/UI/WBP_Foo) to its .uasset file path on disk. */
	static FString GetSourceFilePath(const FString& PackageName);

	/** Compute an MD5 hash of the file at the given path. Returns empty string on failure. */
	static FString ComputeFileHash(const FString& FilePath);

	/** Write audit content to disk. Returns true on success. */
	static bool WriteAuditFile(const FString& Content, const FString& OutputPath);

	/** Write (or overwrite) audit-manifest.json in Saved/Fathom/. */
	static void WriteAuditManifest();

	/**
	 * Returns true if the given Blueprint native class is safe to load and audit.
	 * Some Blueprint subclasses (ControlRig, RigVM) crash during LoadObject because
	 * their subsystems expect a specific loading context we cannot provide.
	 */
	static bool IsSupportedBlueprintClass(const FTopLevelAssetPath& ClassPath);
};

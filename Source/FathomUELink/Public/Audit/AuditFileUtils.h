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
	static constexpr int32 AuditSchemaVersion = 15;

	/** Human-readable type string for a Blueprint variable pin type. */
	static FString GetVariableTypeString(const FEdGraphPinType& PinType);

	/** Return the base directory for all audit files: <ProjectDir>/Saved/Fathom/Audit/v<N>/ */
	static FString GetAuditBaseDir();

	/**
	 * Compute the on-disk output path for an asset's audit file.
	 *  /Game/UI/Widgets/WBP_Foo  ->  <ProjectDir>/Saved/Fathom/Audit/v<N>/UI/Widgets/WBP_Foo.md
	 *  /MyPlugin/Foo/Bar         ->  <ProjectDir>/Saved/Fathom/Audit/v<N>/_Plugins/MyPlugin/Foo/Bar.md
	 */
	static FString GetAuditOutputPath(const UBlueprint* BP);
	static FString GetAuditOutputPath(const FString& PackageName);

	/**
	 * Returns true if the package belongs to a content root we want to audit:
	 * the project's /Game/ root, or the mount point of an enabled project-type plugin.
	 * Engine, Enterprise, External, and Mod plugins are excluded.
	 */
	static bool IsAuditablePackage(const FString& PackageName);

	/**
	 * Reverse of the path mapping in GetAuditOutputPath: given an audit-file path
	 * relative to GetAuditBaseDir() (forward-slash, no .md suffix), return the
	 * corresponding package name. Returns empty on failure.
	 */
	static FString PackageNameFromRelativeAuditPath(const FString& RelPath);

	/** Delete an audit file. Returns true if the file was deleted or did not exist. */
	static bool DeleteAuditFile(const FString& FilePath);

	/** Convert a package name (e.g. /Game/UI/WBP_Foo) to its .uasset file path on disk. */
	static FString GetSourceFilePath(const FString& PackageName);

	/**
	 * If AbsPath is under FPaths::ProjectDir(), return the project-relative path
	 * with forward slashes. Otherwise return the absolute path unchanged.
	 * Used to write a portable SourcePath: header into audit files.
	 */
	static FString ToProjectRelativeSourcePath(const FString& AbsPath);

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

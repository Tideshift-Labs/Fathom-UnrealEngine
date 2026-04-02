#pragma once

#include "CoreMinimal.h"
#include "Audit/AuditTypes.h"

/** A property binding on a StateTree node (e.g. LeftTag <- Parameters.Motivation). */
struct FStateTreePropertyBindingAuditData
{
	FString TargetProperty; // Property name on the node, e.g. "LeftTag"
	FString SourcePath;     // Human-readable source, e.g. "Parameters.Motivation"
};

/** Audit data for a single StateTree editor node (task, condition, evaluator, or consideration). */
struct FStateTreeEditorNodeAuditData
{
	FString Name;
	FString ClassName;
	FString ExpressionOperand; // "AND" / "OR" (for conditions only)
	bool bIsFirstInList = false; // suppress operand on first item
	TArray<FPropertyOverrideData> Properties;
	TArray<FStateTreePropertyBindingAuditData> Bindings;
};

/** Audit data for a single StateTree transition. */
struct FStateTreeTransitionAuditData
{
	FString Trigger;       // "OnStateCompleted", "OnTick", etc.
	FString TargetState;   // Target state name or transition type (Succeeded, Failed, NextState, etc.)
	FString Priority;      // "Normal", "High", "Critical", etc.
	bool bEnabled = true;
	bool bDelayTransition = false;
	float DelayDuration = 0.f;
	float DelayRandomVariance = 0.f;
	TArray<FStateTreeEditorNodeAuditData> Conditions;
};

/** Audit data for a single StateTree state (recursive). */
struct FStateTreeStateAuditData
{
	FString Name;
	FString Type;               // "State", "Group", "Linked", "LinkedAsset", "Subtree"
	bool bEnabled = true;
	FString Description;
	FString Tag;                // Gameplay tag (empty if None)
	FString SelectionBehavior;  // Only serialized when non-default
	FString TasksCompletion;    // "All" or "Any" (only serialized when non-default)
	float Weight = 1.f;
	bool bHasCustomTickRate = false;
	float CustomTickRate = 0.f;
	FString RequiredEventTag;        // Only serialized when set
	FString RequiredEventPayload;    // Payload struct name (if set)
	bool bConsumeEventOnSelect = true; // Only serialized when false
	bool bCheckPrerequisitesWhenActivatingChildDirectly = true; // Only serialized when false
	TArray<FPropertyOverrideData> ParameterOverrides; // Overridden linked asset parameters
	FString LinkedStateName;    // For Linked/LinkedAsset types
	FString LinkedAssetPath;    // For LinkedAsset type
	TArray<FStateTreeEditorNodeAuditData> Tasks;
	TArray<FStateTreeEditorNodeAuditData> EnterConditions;
	TArray<FStateTreeEditorNodeAuditData> Considerations;
	TArray<FStateTreeTransitionAuditData> Transitions;
	TArray<FStateTreeStateAuditData> Children;
};

/** Top-level audit data for a StateTree asset. */
struct FStateTreeAuditData
{
	FString Name;
	FString Path;
	FString PackageName;
	FString SourceFilePath;
	FString OutputPath;
	FString SchemaName;

	TArray<FStateTreeEditorNodeAuditData> Evaluators;
	TArray<FStateTreeEditorNodeAuditData> GlobalTasks;
	TArray<FStateTreeStateAuditData> SubTrees;
};

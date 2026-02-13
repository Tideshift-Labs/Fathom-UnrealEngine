#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintAuditCommandlet.generated.h"

/**
 * Commandlet that analyzes Blueprint assets and outputs a Markdown summary.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=BlueprintAudit [-AssetPath=/Game/Path/To/BP] [-Output=path.md]
 *
 * If -AssetPath is omitted, all Blueprints in the project are audited
 * and each gets its own .md file under Saved/Audit/Blueprints/.
 *
 * If -AssetPath is provided, a single file is written to -Output
 * (defaults to <ProjectDir>/BlueprintAudit.md).
 */
UCLASS()
class FATHOMUELINK_API UBlueprintAuditCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBlueprintAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};

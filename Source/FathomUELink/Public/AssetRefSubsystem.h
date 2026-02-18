#pragma once

#include "CoreMinimal.h"
#include "FathomHttpServer.h"
#include "EditorSubsystem.h"
#include "AssetRefSubsystem.generated.h"

/**
 * Editor subsystem that owns the Fathom HTTP server.
 * Starts the server on editor launch and stops it on shutdown.
 */
UCLASS()
class FATHOMUELINK_API UAssetRefSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	TUniquePtr<FFathomHttpServer> HttpServer;
};

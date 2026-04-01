#include "AssetRefSubsystem.h"

#include "FathomHttpServer.h"
#include "FathomUELinkModule.h"

void UAssetRefSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The HTTP server is only useful in interactive editor sessions.
	// Skip it during commandlet runs (e.g. -run=BlueprintAudit) to avoid
	// port conflicts with a running editor and unnecessary bind errors.
	if (IsRunningCommandlet())
	{
		return;
	}

	HttpServer = MakeUnique<FFathomHttpServer>();
	if (!HttpServer->Start())
	{
		UE_LOG(LogFathomUELink, Warning, TEXT("Fathom: HTTP server failed to start"));
		HttpServer.Reset();
	}
}

void UAssetRefSubsystem::Deinitialize()
{
	if (HttpServer)
	{
		HttpServer->Stop();
		HttpServer.Reset();
	}

	Super::Deinitialize();
}

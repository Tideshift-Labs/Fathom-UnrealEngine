#include "AssetRefSubsystem.h"

#include "FathomHttpServer.h"
#include "BlueprintAuditor.h"

void UAssetRefSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

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

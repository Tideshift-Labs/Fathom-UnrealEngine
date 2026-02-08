#include "AssetRefSubsystem.h"

#include "AssetRefHttpServer.h"
#include "BlueprintAuditor.h"

void UAssetRefSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	HttpServer = MakeUnique<FAssetRefHttpServer>();
	if (!HttpServer->Start())
	{
		UE_LOG(LogCoRider, Warning, TEXT("CoRider: Asset ref HTTP server failed to start"));
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

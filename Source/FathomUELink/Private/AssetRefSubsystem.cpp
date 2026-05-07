#include "AssetRefSubsystem.h"

#include "FathomHttpServer.h"
#include "FathomUELinkModule.h"
#include "Misc/App.h"

void UAssetRefSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The HTTP server is only useful in interactive editor sessions.
	// Skip during commandlets, cook, and unattended runs (UAT packaging)
	// to avoid port-bind conflicts with a concurrently running editor.
	if (IsRunningCommandlet() || IsRunningCookCommandlet() || FApp::IsUnattended())
	{
		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Skipping HTTP server startup during commandlet/cook/unattended execution."));
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

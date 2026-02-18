#include "FathomHttpServer.h"

#include "FathomUELinkModule.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpResultCallback.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

static constexpr int32 PortRangeStart = 19900;
static constexpr int32 PortRangeEnd = 19910;

FFathomHttpServer::FFathomHttpServer()
{
}

FFathomHttpServer::~FFathomHttpServer()
{
	Stop();
}

bool FFathomHttpServer::Start()
{
	for (int32 Port = PortRangeStart; Port <= PortRangeEnd; ++Port)
	{
		if (TryBind(Port))
		{
			BoundPort = Port;
			WriteMarkerFile();
			UE_LOG(LogFathomUELink, Display, TEXT("Fathom: HTTP server listening on port %d"), BoundPort);
			return true;
		}
	}

	UE_LOG(LogFathomUELink, Error, TEXT("Fathom: Failed to bind HTTP server on ports %d-%d"), PortRangeStart, PortRangeEnd);
	return false;
}

void FFathomHttpServer::Stop()
{
	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
		RouteHandles.Empty();
	}

	if (BoundPort != 0)
	{
		DeleteMarkerFile();
		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: HTTP server stopped (was on port %d)"), BoundPort);
		BoundPort = 0;
	}

	HttpRouter.Reset();
}

bool FFathomHttpServer::TryBind(int32 Port)
{
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	TSharedPtr<IHttpRouter> Router = HttpServerModule.GetHttpRouter(Port);
	if (!Router.IsValid())
	{
		return false;
	}

	TArray<FHttpRouteHandle> Handles;

	// -- Asset reference routes --

	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/health")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FFathomHttpServer::HandleHealth)));

	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/dependencies")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FFathomHttpServer::HandleDependencies)));

	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/referencers")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FFathomHttpServer::HandleReferencers)));

	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/search")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FFathomHttpServer::HandleSearch)));

	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/show")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FFathomHttpServer::HandleShow)));

	// -- Live Coding routes --

	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/live-coding/status")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FFathomHttpServer::HandleLiveCodingStatus)));

	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/live-coding/compile")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FFathomHttpServer::HandleLiveCodingCompile)));

	// Check all handles are valid
	for (const FHttpRouteHandle& Handle : Handles)
	{
		if (!Handle.IsValid())
		{
			// Unbind any that succeeded and bail
			for (const FHttpRouteHandle& H : Handles)
			{
				if (H.IsValid())
				{
					Router->UnbindRoute(H);
				}
			}
			return false;
		}
	}

	HttpServerModule.StartAllListeners();

	HttpRouter = Router;
	RouteHandles = MoveTemp(Handles);
	return true;
}

void FFathomHttpServer::WriteMarkerFile() const
{
	const FString MarkerPath = GetMarkerFilePath();
	const FString MarkerDir = FPaths::GetPath(MarkerPath);
	IFileManager::Get().MakeDirectory(*MarkerDir, true);

	const FString Json = FString::Printf(
		TEXT("{\n  \"port\": %d,\n  \"pid\": %d,\n  \"started\": \"%s\"\n}"),
		BoundPort,
		FPlatformProcess::GetCurrentProcessId(),
		*FDateTime::Now().ToIso8601());

	FFileHelper::SaveStringToFile(Json, *MarkerPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FFathomHttpServer::DeleteMarkerFile() const
{
	const FString MarkerPath = GetMarkerFilePath();
	IFileManager::Get().Delete(*MarkerPath, false, false, true);
}

FString FFathomHttpServer::GetMarkerFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Fathom"), TEXT(".fathom-ue-server.json"));
}

// -- Health --

bool FFathomHttpServer::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetStringField(TEXT("status"), TEXT("ok"));
	ResponseJson->SetNumberField(TEXT("port"), BoundPort);
	ResponseJson->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(ResponseJson, Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	OnComplete(MoveTemp(Response));
	return true;
}

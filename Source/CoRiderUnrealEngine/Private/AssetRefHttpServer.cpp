#include "AssetRefHttpServer.h"

#include "BlueprintAuditor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpResultCallback.h"
#include "HttpServerResponse.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

static constexpr int32 PortRangeStart = 19877;
static constexpr int32 PortRangeEnd = 19887;

static FString GetDependencyTypeString(UE::AssetRegistry::EDependencyProperty Properties)
{
	using namespace UE::AssetRegistry;

	if (Properties == EDependencyProperty::None)
	{
		return TEXT("Other");
	}
	if (EnumHasAnyFlags(Properties, EDependencyProperty::Hard))
	{
		return TEXT("Hard");
	}
	return TEXT("Soft");
}

static FString GetDependencyCategoryString(UE::AssetRegistry::EDependencyCategory Category)
{
	using namespace UE::AssetRegistry;

	switch (Category)
	{
	case EDependencyCategory::Package:
		return TEXT("Package");
	case EDependencyCategory::SearchableName:
		return TEXT("SearchableName");
	case EDependencyCategory::Manage:
		return TEXT("Manage");
	default:
		return TEXT("Unknown");
	}
}

FAssetRefHttpServer::FAssetRefHttpServer()
{
}

FAssetRefHttpServer::~FAssetRefHttpServer()
{
	Stop();
}

bool FAssetRefHttpServer::Start()
{
	for (int32 Port = PortRangeStart; Port <= PortRangeEnd; ++Port)
	{
		if (TryBind(Port))
		{
			BoundPort = Port;
			WriteMarkerFile();
			UE_LOG(LogCoRider, Display, TEXT("CoRider: Asset ref HTTP server listening on port %d"), BoundPort);
			return true;
		}
	}

	UE_LOG(LogCoRider, Error, TEXT("CoRider: Failed to bind asset ref HTTP server on ports %d-%d"), PortRangeStart, PortRangeEnd);
	return false;
}

void FAssetRefHttpServer::Stop()
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
		FHttpServerModule::Get().StopAllListeners();
		DeleteMarkerFile();
		UE_LOG(LogCoRider, Display, TEXT("CoRider: Asset ref HTTP server stopped (was on port %d)"), BoundPort);
		BoundPort = 0;
	}

	HttpRouter.Reset();
}

bool FAssetRefHttpServer::TryBind(int32 Port)
{
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	TSharedPtr<IHttpRouter> Router = HttpServerModule.GetHttpRouter(Port);
	if (!Router.IsValid())
	{
		return false;
	}

	TArray<FHttpRouteHandle> Handles;

	// GET /asset-refs/health
	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/health")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FAssetRefHttpServer::HandleHealth)));

	// GET /asset-refs/dependencies
	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/dependencies")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FAssetRefHttpServer::HandleDependencies)));

	// GET /asset-refs/referencers
	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/referencers")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FAssetRefHttpServer::HandleReferencers)));

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

void FAssetRefHttpServer::WriteMarkerFile() const
{
	const FString MarkerPath = GetMarkerFilePath();
	const FString Json = FString::Printf(
		TEXT("{\n  \"port\": %d,\n  \"pid\": %d,\n  \"started\": \"%s\"\n}"),
		BoundPort,
		FPlatformProcess::GetCurrentProcessId(),
		*FDateTime::Now().ToIso8601());

	FFileHelper::SaveStringToFile(Json, *MarkerPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FAssetRefHttpServer::DeleteMarkerFile() const
{
	const FString MarkerPath = GetMarkerFilePath();
	IFileManager::Get().Delete(*MarkerPath, false, false, true);
}

FString FAssetRefHttpServer::GetMarkerFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT(".corider-ue-server.json"));
}

// -- Route handlers --

bool FAssetRefHttpServer::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
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

bool FAssetRefHttpServer::HandleDependencies(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return HandleAssetQuery(Request, OnComplete, true);
}

bool FAssetRefHttpServer::HandleReferencers(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return HandleAssetQuery(Request, OnComplete, false);
}

bool FAssetRefHttpServer::HandleAssetQuery(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, bool bGetDependencies)
{
	// Extract ?asset= query parameter
	FString AssetPath;
	if (Request.QueryParams.Contains(TEXT("asset")))
	{
		AssetPath = Request.QueryParams[TEXT("asset")];
	}

	// Normalize: strip object name suffix if present (e.g. "/Game/Foo/Bar.Bar" -> "/Game/Foo/Bar")
	int32 DotIndex;
	if (AssetPath.FindLastChar(TEXT('.'), DotIndex))
	{
		AssetPath.LeftInline(DotIndex);
	}

	if (AssetPath.IsEmpty())
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("error"), TEXT("Missing required 'asset' query parameter"));
		ErrorJson->SetStringField(TEXT("usage"), bGetDependencies
			? TEXT("/asset-refs/dependencies?asset=/Game/Path/To/Asset")
			: TEXT("/asset-refs/referencers?asset=/Game/Path/To/Asset"));

		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(ErrorJson, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::BadRequest;
		OnComplete(MoveTemp(Response));
		return true;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetDependency> Results;
	if (bGetDependencies)
	{
		Registry.GetDependencies(FAssetIdentifier(FName(*AssetPath)), Results, UE::AssetRegistry::EDependencyCategory::All);
	}
	else
	{
		Registry.GetReferencers(FAssetIdentifier(FName(*AssetPath)), Results, UE::AssetRegistry::EDependencyCategory::All);
	}

	// Build response JSON
	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetStringField(TEXT("asset"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (const FAssetDependency& Dep : Results)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package"), Dep.AssetId.PackageName.ToString());
		Entry->SetStringField(TEXT("category"), GetDependencyCategoryString(Dep.Category));
		Entry->SetStringField(TEXT("type"), GetDependencyTypeString(Dep.Properties));
		EntriesArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	const FString FieldName = bGetDependencies ? TEXT("dependencies") : TEXT("referencers");
	ResponseJson->SetArrayField(FieldName, EntriesArray);

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(ResponseJson, Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	OnComplete(MoveTemp(Response));
	return true;
}


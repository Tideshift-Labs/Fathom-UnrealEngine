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

static constexpr int32 PortRangeStart = 19900;
static constexpr int32 PortRangeEnd = 19910;

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

	// GET /asset-refs/search
	Handles.Add(Router->BindRoute(
		FHttpPath(TEXT("/asset-refs/search")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FAssetRefHttpServer::HandleSearch)));

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

	// Check if this package actually exists in the registry
	TArray<FAssetData> AssetDataList;
	Registry.GetAssetsByPackageName(FName(*AssetPath), AssetDataList, true);
	if (AssetDataList.IsEmpty())
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("error"), TEXT("Asset not found in registry"));
		ErrorJson->SetStringField(TEXT("asset"), AssetPath);
		ErrorJson->SetStringField(TEXT("hint"), TEXT("Check that the package path is correct and the asset is loaded"));

		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(ErrorJson, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::NotFound;
		OnComplete(MoveTemp(Response));
		return true;
	}

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

		// Look up asset class from registry (e.g. "Texture2D", "WidgetBlueprint")
		TArray<FAssetData> DepAssets;
		Registry.GetAssetsByPackageName(Dep.AssetId.PackageName, DepAssets, true);
		if (!DepAssets.IsEmpty())
		{
			Entry->SetStringField(TEXT("assetClass"), DepAssets[0].AssetClassPath.GetAssetName().ToString());
		}

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

bool FAssetRefHttpServer::HandleSearch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Parse query parameters
	FString Query;
	if (Request.QueryParams.Contains(TEXT("q")))
	{
		Query = Request.QueryParams[TEXT("q")];
	}

	if (Query.IsEmpty())
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("error"), TEXT("Missing required 'q' query parameter"));
		ErrorJson->SetStringField(TEXT("usage"), TEXT("/asset-refs/search?q=term&class=WidgetBlueprint&limit=50"));

		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(ErrorJson, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::BadRequest;
		OnComplete(MoveTemp(Response));
		return true;
	}

	FString ClassFilter;
	if (Request.QueryParams.Contains(TEXT("class")))
	{
		ClassFilter = Request.QueryParams[TEXT("class")];
	}

	FString PathPrefix;
	if (Request.QueryParams.Contains(TEXT("pathPrefix")))
	{
		PathPrefix = Request.QueryParams[TEXT("pathPrefix")];
	}

	int32 Limit = 50;
	if (Request.QueryParams.Contains(TEXT("limit")))
	{
		Limit = FCString::Atoi(*Request.QueryParams[TEXT("limit")]);
		if (Limit <= 0) Limit = 50;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Build FARFilter so the registry handles path and class filtering internally,
	// avoiding iteration over engine/plugin assets entirely.
	FARFilter Filter;
	Filter.bIncludeOnlyOnDiskAssets = true;

	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathPrefix));
		Filter.bRecursivePaths = true;
	}

	// Resolve class filter to FTopLevelAssetPath for the registry filter.
	// If resolution fails (typo, module not loaded), fall back to manual filtering.
	bool bClassInFilter = false;
	if (!ClassFilter.IsEmpty())
	{
		UClass* ResolvedClass = FindFirstObject<UClass>(*ClassFilter, EFindFirstObjectOptions::NativeFirst);
		if (ResolvedClass)
		{
			Filter.ClassPaths.Add(ResolvedClass->GetClassPathName());
			bClassInFilter = true;
		}
	}

	// Split query into tokens for multi-word matching (all tokens must match)
	TArray<FString> Tokens;
	Query.ToLower().ParseIntoArrayWS(Tokens);

	// Score and collect matching assets via callback (no bulk TArray copy)
	struct FScoredAsset
	{
		FAssetData AssetData;
		int32 Score;
	};
	TArray<FScoredAsset> ScoredResults;

	// If class wasn't resolved into the filter, fall back to manual class matching
	const bool bManualClassFilter = !ClassFilter.IsEmpty() && !bClassInFilter;

	auto ScoreAsset = [&Tokens, &ScoredResults, bManualClassFilter, &ClassFilter](const FAssetData& Asset) -> bool
	{
		// Manual class filter fallback when UClass couldn't be resolved
		if (bManualClassFilter)
		{
			FString AssetClassName = Asset.AssetClassPath.GetAssetName().ToString();
			if (!AssetClassName.Equals(ClassFilter, ESearchCase::IgnoreCase))
			{
				return true; // skip, continue enumeration
			}
		}

		FString AssetName = Asset.AssetName.ToString().ToLower();
		FString PackageName = Asset.PackageName.ToString().ToLower();

		// Score each token, take the minimum. All tokens must match somewhere.
		int32 MinScore = MAX_int32;
		bool bAllMatched = true;

		for (const FString& Token : Tokens)
		{
			int32 TokenScore = -1;

			if (AssetName.Equals(Token))
			{
				TokenScore = 3; // Exact name match
			}
			else if (AssetName.StartsWith(Token))
			{
				TokenScore = 2; // Name prefix
			}
			else if (AssetName.Contains(Token))
			{
				TokenScore = 1; // Name substring
			}
			else if (PackageName.Contains(Token))
			{
				TokenScore = 0; // Path-only match
			}

			if (TokenScore < 0)
			{
				bAllMatched = false;
				break;
			}

			MinScore = FMath::Min(MinScore, TokenScore);
		}

		if (bAllMatched && Tokens.Num() > 0)
		{
			ScoredResults.Add({ Asset, MinScore });
		}

		return true; // continue enumeration
	};

	if (Filter.IsEmpty())
	{
		Registry.EnumerateAllAssets(ScoreAsset);
	}
	else
	{
		Registry.EnumerateAssets(Filter, ScoreAsset);
	}

	// Sort by score descending
	ScoredResults.Sort([](const FScoredAsset& A, const FScoredAsset& B)
	{
		return A.Score > B.Score;
	});

	// Cap at limit
	if (ScoredResults.Num() > Limit)
	{
		ScoredResults.SetNum(Limit);
	}

	// Build response
	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetStringField(TEXT("query"), Query);

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	for (const FScoredAsset& Scored : ScoredResults)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package"), Scored.AssetData.PackageName.ToString());
		Entry->SetStringField(TEXT("name"), Scored.AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("assetClass"), Scored.AssetData.AssetClassPath.GetAssetName().ToString());
		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	ResponseJson->SetArrayField(TEXT("results"), ResultsArray);

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(ResponseJson, Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	OnComplete(MoveTemp(Response));
	return true;
}


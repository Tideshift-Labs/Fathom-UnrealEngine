#include "FathomHttpServer.h"

#include "BlueprintAuditor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpServerRequest.h"
#include "HttpResultCallback.h"
#include "HttpServerResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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

bool FFathomHttpServer::HandleDependencies(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return HandleAssetQuery(Request, OnComplete, true);
}

bool FFathomHttpServer::HandleReferencers(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return HandleAssetQuery(Request, OnComplete, false);
}

bool FFathomHttpServer::HandleAssetQuery(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, bool bGetDependencies)
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

bool FFathomHttpServer::HandleSearch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Parse query parameters
	FString Query;
	if (Request.QueryParams.Contains(TEXT("q")))
	{
		Query = Request.QueryParams[TEXT("q")];
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

	// Require at least one of: query, class filter, or path prefix
	if (Query.IsEmpty() && ClassFilter.IsEmpty() && PathPrefix.IsEmpty())
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("error"), TEXT("Provide a 'q' search term and/or filters ('class', 'pathPrefix')"));
		ErrorJson->SetStringField(TEXT("usage"), TEXT("/asset-refs/search?q=term or /asset-refs/search?class=WidgetBlueprint&pathPrefix=/Game/UI"));

		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(ErrorJson, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::BadRequest;
		OnComplete(MoveTemp(Response));
		return true;
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

		if (Tokens.Num() == 0)
		{
			// Browse mode: no search query, accept all assets that pass the filter
			ScoredResults.Add({ Asset, 0 });
		}
		else if (bAllMatched)
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

bool FFathomHttpServer::HandleShow(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FString PackagePath;
	if (Request.QueryParams.Contains(TEXT("package")))
	{
		PackagePath = Request.QueryParams[TEXT("package")];
	}

	if (PackagePath.IsEmpty())
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("error"), TEXT("Missing required 'package' query parameter"));
		ErrorJson->SetStringField(TEXT("usage"), TEXT("/asset-refs/show?package=/Game/Path/To/Asset"));

		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(ErrorJson, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::BadRequest;
		OnComplete(MoveTemp(Response));
		return true;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> AssetDataList;
	Registry.GetAssetsByPackageName(FName(*PackagePath), AssetDataList, true);
	if (AssetDataList.IsEmpty())
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("error"), TEXT("Asset not found in registry"));
		ErrorJson->SetStringField(TEXT("package"), PackagePath);

		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(ErrorJson, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::NotFound;
		OnComplete(MoveTemp(Response));
		return true;
	}

	const FAssetData& Asset = AssetDataList[0];

	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetStringField(TEXT("package"), Asset.PackageName.ToString());
	ResponseJson->SetStringField(TEXT("name"), Asset.AssetName.ToString());
	ResponseJson->SetStringField(TEXT("assetClass"), Asset.AssetClassPath.GetAssetName().ToString());

	// On-disk path and file size
	FString DiskPath = FBlueprintAuditor::GetSourceFilePath(PackagePath);
	if (!DiskPath.IsEmpty())
	{
		ResponseJson->SetStringField(TEXT("diskPath"), DiskPath);
		const int64 FileSize = IFileManager::Get().FileSize(*DiskPath);
		if (FileSize >= 0)
		{
			ResponseJson->SetNumberField(TEXT("diskSizeBytes"), static_cast<double>(FileSize));
		}
	}

	// Dependency and referencer counts
	TArray<FAssetDependency> Dependencies;
	Registry.GetDependencies(FAssetIdentifier(FName(*PackagePath)), Dependencies, UE::AssetRegistry::EDependencyCategory::All);
	ResponseJson->SetNumberField(TEXT("dependencyCount"), Dependencies.Num());

	TArray<FAssetDependency> Referencers;
	Registry.GetReferencers(FAssetIdentifier(FName(*PackagePath)), Referencers, UE::AssetRegistry::EDependencyCategory::All);
	ResponseJson->SetNumberField(TEXT("referencerCount"), Referencers.Num());

	// Registry tags (skip FiBData which contains binary blob data)
	TSharedRef<FJsonObject> TagsJson = MakeShared<FJsonObject>();
	for (const auto& TagPair : Asset.TagsAndValues)
	{
		const FString Key = TagPair.Key.ToString();
		if (Key == TEXT("FiBData"))
		{
			continue;
		}
		TagsJson->SetStringField(Key, TagPair.Value.GetValue());
	}
	ResponseJson->SetObjectField(TEXT("tags"), TagsJson);

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(ResponseJson, Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	OnComplete(MoveTemp(Response));
	return true;
}

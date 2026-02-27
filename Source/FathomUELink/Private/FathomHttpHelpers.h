#pragma once

#include "Dom/JsonObject.h"
#include "HttpResultCallback.h"
#include "HttpServerResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace FathomHttp
{
	/** Serialize a JsonObject to a JSON string, wrap it in an FHttpServerResponse, and call OnComplete. */
	inline bool SendJson(const FHttpResultCallback& OnComplete, TSharedRef<FJsonObject> Json,
		EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(Json, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = Code;
		OnComplete(MoveTemp(Response));
		return true;
	}

	/** Build {"error":"...", "usage":"..."} and send with the given status code. Usage is optional. */
	inline bool SendError(const FHttpResultCallback& OnComplete, EHttpServerResponseCodes Code,
		const FString& Message, const FString& Usage = FString())
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("error"), Message);
		if (!Usage.IsEmpty())
		{
			Json->SetStringField(TEXT("usage"), Usage);
		}
		return SendJson(OnComplete, Json, Code);
	}
}

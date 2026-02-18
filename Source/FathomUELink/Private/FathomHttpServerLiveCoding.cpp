#include "FathomHttpServer.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpServerRequest.h"
#include "HttpResultCallback.h"
#include "HttpServerResponse.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

#if PLATFORM_WINDOWS
/**
 * Captures log lines from the LogLiveCoding category during a compile operation.
 * Thread-safe: Serialize() may be called from any thread by the logging system.
 */
class FLiveCodingLogCapture : public FOutputDevice
{
public:
	void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (Category == TEXT("LogLiveCoding"))
		{
			FScopeLock Lock(&CriticalSection);
			CapturedLines.Add(FString(Message));
		}
	}

	TArray<FString> GetCapturedLines()
	{
		FScopeLock Lock(&CriticalSection);
		return CapturedLines;
	}

private:
	FCriticalSection CriticalSection;
	TArray<FString> CapturedLines;
};
#endif

bool FFathomHttpServer::HandleLiveCodingStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);

	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetBoolField(TEXT("hasStarted"), LiveCoding != nullptr && LiveCoding->HasStarted());
	ResponseJson->SetBoolField(TEXT("isEnabledForSession"), LiveCoding != nullptr && LiveCoding->IsEnabledForSession());
	ResponseJson->SetBoolField(TEXT("isCompiling"), LiveCoding != nullptr && LiveCoding->IsCompiling());

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(ResponseJson, Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	OnComplete(MoveTemp(Response));
	return true;
#else
	TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
	ErrorJson->SetStringField(TEXT("error"), TEXT("Live Coding is only available on Windows"));

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(ErrorJson, Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::NotSupported;
	OnComplete(MoveTemp(Response));
	return true;
#endif
}

bool FFathomHttpServer::HandleLiveCodingCompile(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);

	if (LiveCoding == nullptr || !LiveCoding->HasStarted())
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("result"), TEXT("NotStarted"));
		ErrorJson->SetStringField(TEXT("resultText"), TEXT("Live Coding has not been started. Enable Live Coding in the editor and ensure it has started."));

		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(ErrorJson, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::BadRequest;
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (LiveCoding->IsCompiling())
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("result"), TEXT("AlreadyCompiling"));
		ErrorJson->SetStringField(TEXT("resultText"), TEXT("A Live Coding compile is already in progress."));

		FString Body;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(ErrorJson, Writer);

		auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::BadRequest;
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Set up log capture with RAII cleanup
	FLiveCodingLogCapture LogCapture;
	GLog->AddOutputDevice(&LogCapture);
	ON_SCOPE_EXIT { GLog->RemoveOutputDevice(&LogCapture); };

	const double StartTime = FPlatformTime::Seconds();

	// Compile with WaitForCompletion - this blocks until LiveCodingConsole.exe finishes
	ELiveCodingCompileResult Result = ELiveCodingCompileResult::Failure;
	LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);

	const double Duration = FPlatformTime::Seconds() - StartTime;

	// Map result to string
	FString ResultString;
	FString ResultText;
	switch (Result)
	{
	case ELiveCodingCompileResult::Success:
		ResultString = TEXT("Success");
		ResultText = TEXT("Live coding succeeded");
		break;
	case ELiveCodingCompileResult::Failure:
		ResultString = TEXT("Failure");
		ResultText = TEXT("Live coding compile failed");
		break;
	case ELiveCodingCompileResult::NoChanges:
		ResultString = TEXT("NoChanges");
		ResultText = TEXT("No code changes detected");
		break;
	case ELiveCodingCompileResult::Cancelled:
		ResultString = TEXT("Cancelled");
		ResultText = TEXT("Live coding compile was cancelled");
		break;
	default:
		ResultString = TEXT("Unknown");
		ResultText = TEXT("Unexpected compile result");
		break;
	}

	// Build response
	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetStringField(TEXT("result"), ResultString);
	ResponseJson->SetStringField(TEXT("resultText"), ResultText);

	TArray<FString> Lines = LogCapture.GetCapturedLines();
	TArray<TSharedPtr<FJsonValue>> LogArray;
	for (const FString& Line : Lines)
	{
		LogArray.Add(MakeShared<FJsonValueString>(Line));
	}
	ResponseJson->SetArrayField(TEXT("logs"), LogArray);
	ResponseJson->SetNumberField(TEXT("durationMs"), FMath::RoundToInt(Duration * 1000.0));

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(ResponseJson, Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	OnComplete(MoveTemp(Response));
	return true;
#else
	TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
	ErrorJson->SetStringField(TEXT("error"), TEXT("Live Coding is only available on Windows"));

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(ErrorJson, Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::NotSupported;
	OnComplete(MoveTemp(Response));
	return true;
#endif
}

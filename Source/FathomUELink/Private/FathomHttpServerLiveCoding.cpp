#include "FathomHttpServer.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpServerRequest.h"
#include "HttpResultCallback.h"
#include "HttpServerResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#include "Async/Async.h"
#endif

// ---------------------------------------------------------------------------
// Log capture (Windows only)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Compile state shared between the background thread and HTTP handlers.
// Protected by GCompileStateLock. Handlers run on the game thread (via
// FHttpServerModule::Tick), so the lock is only contended briefly when
// the background compile thread writes its result.
// ---------------------------------------------------------------------------

static FCriticalSection GCompileStateLock;

// In-flight tracking
static bool GCompileInFlight = false;
static TUniquePtr<FLiveCodingLogCapture> GActiveLogCapture;
static double GCompileStartTime = 0.0;

// Last completed result (persists until the next compile starts)
static bool GHasLastCompileResult = false;
static FString GLastCompileResult;
static FString GLastCompileResultText;
static TArray<FString> GLastCompileLogs;
static int32 GLastCompileDurationMs = 0;

static void MapCompileResult(ELiveCodingCompileResult Result, FString& OutResult, FString& OutResultText)
{
	switch (Result)
	{
	case ELiveCodingCompileResult::Success:
		OutResult = TEXT("Success");
		OutResultText = TEXT("Live coding succeeded");
		break;
	case ELiveCodingCompileResult::Failure:
		OutResult = TEXT("Failure");
		OutResultText = TEXT("Live coding compile failed");
		break;
	case ELiveCodingCompileResult::NoChanges:
		OutResult = TEXT("NoChanges");
		OutResultText = TEXT("No code changes detected");
		break;
	case ELiveCodingCompileResult::Cancelled:
		OutResult = TEXT("Cancelled");
		OutResultText = TEXT("Live coding compile was cancelled");
		break;
	default:
		OutResult = TEXT("Unknown");
		OutResultText = TEXT("Unexpected compile result");
		break;
	}
}

#endif // PLATFORM_WINDOWS

// ---------------------------------------------------------------------------
// GET /live-coding/status
// ---------------------------------------------------------------------------

bool FFathomHttpServer::HandleLiveCodingStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);

	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetBoolField(TEXT("hasStarted"), LiveCoding != nullptr && LiveCoding->HasStarted());
	ResponseJson->SetBoolField(TEXT("isEnabledForSession"), LiveCoding != nullptr && LiveCoding->IsEnabledForSession());
	ResponseJson->SetBoolField(TEXT("isCompiling"), LiveCoding != nullptr && LiveCoding->IsCompiling());

	// Include last compile result if available
	{
		FScopeLock Lock(&GCompileStateLock);
		ResponseJson->SetBoolField(TEXT("compileInFlight"), GCompileInFlight);

		if (GHasLastCompileResult)
		{
			TSharedRef<FJsonObject> LastCompile = MakeShared<FJsonObject>();
			LastCompile->SetStringField(TEXT("result"), GLastCompileResult);
			LastCompile->SetStringField(TEXT("resultText"), GLastCompileResultText);

			TArray<TSharedPtr<FJsonValue>> LogArray;
			for (const FString& Line : GLastCompileLogs)
			{
				LogArray.Add(MakeShared<FJsonValueString>(Line));
			}
			LastCompile->SetArrayField(TEXT("logs"), LogArray);
			LastCompile->SetNumberField(TEXT("durationMs"), GLastCompileDurationMs);

			ResponseJson->SetObjectField(TEXT("lastCompile"), LastCompile);
		}
	}

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

// ---------------------------------------------------------------------------
// GET /live-coding/compile
// ---------------------------------------------------------------------------

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

	{
		FScopeLock Lock(&GCompileStateLock);

		if (GCompileInFlight)
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

		// Clear previous result and start new compile
		GHasLastCompileResult = false;
		GCompileInFlight = true;
		GCompileStartTime = FPlatformTime::Seconds();
		GActiveLogCapture = MakeUnique<FLiveCodingLogCapture>();
		GLog->AddOutputDevice(GActiveLogCapture.Get());
	}

	// Dispatch compile to a background thread. Compile(WaitForCompletion)
	// communicates with LiveCodingConsole.exe via IPC and blocks until done.
	// This must NOT run on the game thread (where HTTP handlers execute).
	Async(EAsyncExecution::ThreadPool, [LiveCoding]()
	{
		ELiveCodingCompileResult Result = ELiveCodingCompileResult::Failure;
		LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);

		FScopeLock Lock(&GCompileStateLock);

		const double Duration = FPlatformTime::Seconds() - GCompileStartTime;

		// Unregister log capture and collect lines
		if (GActiveLogCapture.IsValid())
		{
			GLog->RemoveOutputDevice(GActiveLogCapture.Get());
			GLastCompileLogs = GActiveLogCapture->GetCapturedLines();
			GActiveLogCapture.Reset();
		}

		MapCompileResult(Result, GLastCompileResult, GLastCompileResultText);
		GLastCompileDurationMs = FMath::RoundToInt(Duration * 1000.0);
		GHasLastCompileResult = true;
		GCompileInFlight = false;

		UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Live Coding compile finished: %s (%dms)"),
			*GLastCompileResult, GLastCompileDurationMs);
	});

	// Return immediately
	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetStringField(TEXT("result"), TEXT("CompileStarted"));
	ResponseJson->SetStringField(TEXT("resultText"), TEXT("Live Coding compile initiated. Poll /live-coding/status for results."));

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

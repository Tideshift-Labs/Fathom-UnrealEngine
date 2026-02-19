#include "FathomHttpServer.h"

#include "FathomUELinkModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpServerRequest.h"
#include "HttpResultCallback.h"
#include "HttpServerResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
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

/**
 * Returns the path to the UBT log file: %LOCALAPPDATA%\UnrealBuildTool\Log.txt.
 * Returns empty string if LOCALAPPDATA is not set.
 */
static FString GetUBTLogPath()
{
	FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (LocalAppData.IsEmpty())
	{
		return FString();
	}
	return FPaths::Combine(LocalAppData, TEXT("UnrealBuildTool"), TEXT("Log.txt"));
}

/**
 * Reads lines appended to LogPath after PreviousSize, capped at 64KB.
 * Returns empty array on any error (missing file, locked file, nothing new).
 */
static TArray<FString> ReadUBTLogTail(const FString& LogPath, int64 PreviousSize)
{
	TArray<FString> Lines;

	if (LogPath.IsEmpty())
	{
		return Lines;
	}

	const int64 CurrentSize = IFileManager::Get().FileSize(*LogPath);
	if (CurrentSize < 0 || CurrentSize <= PreviousSize)
	{
		return Lines;
	}

	const int64 MaxRead = 64 * 1024;
	int64 ReadOffset = PreviousSize;
	int64 BytesToRead = CurrentSize - PreviousSize;
	if (BytesToRead > MaxRead)
	{
		// Only read the last 64KB of the new content
		ReadOffset = CurrentSize - MaxRead;
		BytesToRead = MaxRead;
	}

	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*LogPath));
	if (!Reader)
	{
		return Lines;
	}

	Reader->Seek(ReadOffset);

	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(BytesToRead);
	Reader->Serialize(Buffer.GetData(), BytesToRead);
	Reader->Close();

	FString Content;
	FFileHelper::BufferToString(Content, Buffer.GetData(), Buffer.Num());

	Content.ParseIntoArrayLines(Lines, /* CullEmpty */ false);

	// Remove empty trailing line from final newline
	if (Lines.Num() > 0 && Lines.Last().IsEmpty())
	{
		Lines.Pop();
	}

	return Lines;
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
//
// Runs Compile(WaitForCompletion) synchronously on the game thread. This is
// the same behavior as pressing Ctrl+Alt+F11 in the editor: the editor
// freezes while LiveCodingConsole.exe compiles, then resumes when done.
// The HTTP response blocks for the duration of the compile (typically 2-30s).
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

	// Set up log capture before compile
	FLiveCodingLogCapture LogCapture;
	GLog->AddOutputDevice(&LogCapture);

	// Snapshot UBT log size so we can read only new content on failure
	const FString UBTLogPath = GetUBTLogPath();
	const int64 UBTLogSizeBefore = UBTLogPath.IsEmpty() ? -1 : IFileManager::Get().FileSize(*UBTLogPath);

	const double StartTime = FPlatformTime::Seconds();

	// Compile synchronously on the game thread. This blocks the editor
	// (same as the Ctrl+Alt+F11 hotkey) while LiveCodingConsole.exe runs.
	ELiveCodingCompileResult Result = ELiveCodingCompileResult::Failure;
	LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);

	// Unregister log capture
	GLog->RemoveOutputDevice(&LogCapture);

	const int32 DurationMs = FMath::RoundToInt((FPlatformTime::Seconds() - StartTime) * 1000.0);

	FString ResultStr, ResultText;
	MapCompileResult(Result, ResultStr, ResultText);

	UE_LOG(LogFathomUELink, Display, TEXT("Fathom: Live Coding compile finished: %s (%dms)"),
		*ResultStr, DurationMs);

	// Build response
	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetStringField(TEXT("result"), ResultStr);
	ResponseJson->SetStringField(TEXT("resultText"), ResultText);
	ResponseJson->SetNumberField(TEXT("durationMs"), DurationMs);

	TArray<FString> CapturedLines = LogCapture.GetCapturedLines();
	TArray<TSharedPtr<FJsonValue>> LogArray;
	for (const FString& Line : CapturedLines)
	{
		LogArray.Add(MakeShared<FJsonValueString>(Line));
	}
	ResponseJson->SetArrayField(TEXT("logs"), LogArray);

	// On failure, read compiler/linker errors from the UBT log
	if (Result == ELiveCodingCompileResult::Failure)
	{
		TArray<FString> UBTLines = ReadUBTLogTail(UBTLogPath, UBTLogSizeBefore);
		if (UBTLines.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorArray;
			for (const FString& Line : UBTLines)
			{
				ErrorArray.Add(MakeShared<FJsonValueString>(Line));
			}
			ResponseJson->SetArrayField(TEXT("buildErrors"), ErrorArray);
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

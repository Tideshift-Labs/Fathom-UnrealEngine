#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"

class IHttpRouter;

/**
 * Lightweight HTTP server exposing IAssetRegistry dependency/referencer queries.
 * Binds to a dynamic port (19877-19887) and writes a marker file so the Rider
 * plugin can discover it.
 */
class FATHOMUELINK_API FAssetRefHttpServer
{
public:
	FAssetRefHttpServer();
	~FAssetRefHttpServer();

	/** Bind to a port, register routes, write marker file. */
	bool Start();

	/** Unbind routes, delete marker file. */
	void Stop();

	/** The port the server is currently listening on, or 0 if not started. */
	int32 GetPort() const { return BoundPort; }

private:
	/** Try to bind the HTTP module router on the given port. */
	bool TryBind(int32 Port);

	/** Write Saved/.fathom-ue-server.json with port, PID, and timestamp. */
	void WriteMarkerFile() const;

	/** Delete Saved/.fathom-ue-server.json if it exists. */
	void DeleteMarkerFile() const;

	/** Return the full path to the marker file. */
	static FString GetMarkerFilePath();

	// Route handlers
	bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDependencies(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleReferencers(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleSearch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleShow(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Shared logic for dependencies/referencers. bGetDependencies=true for deps, false for referencers. */
	bool HandleAssetQuery(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, bool bGetDependencies);

	int32 BoundPort = 0;
	TSharedPtr<IHttpRouter> HttpRouter;
	TArray<FHttpRouteHandle> RouteHandles;
};

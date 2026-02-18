#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"

class IHttpRouter;

/**
 * Fathom UE HTTP server. Exposes endpoints for asset queries, live coding,
 * and other editor functionality over HTTP (ports 19900-19910).
 * Writes a marker file so the Rider plugin can discover and proxy requests.
 */
class FATHOMUELINK_API FFathomHttpServer
{
public:
	FFathomHttpServer();
	~FFathomHttpServer();

	/** Bind to a port, register routes, write marker file. */
	bool Start();

	/** Unbind routes, delete marker file. */
	void Stop();

	/** The port the server is currently listening on, or 0 if not started. */
	int32 GetPort() const { return BoundPort; }

private:
	/** Try to bind the HTTP module router on the given port. */
	bool TryBind(int32 Port);

	/** Write Saved/Fathom/.fathom-ue-server.json with port, PID, and timestamp. */
	void WriteMarkerFile() const;

	/** Delete Saved/Fathom/.fathom-ue-server.json if it exists. */
	void DeleteMarkerFile() const;

	/** Return the full path to the marker file. */
	static FString GetMarkerFilePath();

	// -- Asset reference handlers --
	bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDependencies(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleReferencers(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleSearch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleShow(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Shared logic for dependencies/referencers. bGetDependencies=true for deps, false for referencers. */
	bool HandleAssetQuery(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, bool bGetDependencies);

	// -- Live Coding handlers --
	bool HandleLiveCodingStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleLiveCodingCompile(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	int32 BoundPort = 0;
	TSharedPtr<IHttpRouter> HttpRouter;
	TArray<FHttpRouteHandle> RouteHandles;
};

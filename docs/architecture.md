# Architecture

```
Fathom-UnrealEngine/
└── Source/FathomUELink/
    ├── FathomUELink.Build.cs                    # Module build rules
    ├── Public/
    │   ├── FathomUELinkModule.h                 # Module interface
    │   ├── BlueprintAuditor.h                   # Core audit logic + AuditSchemaVersion
    │   ├── BlueprintAuditCommandlet.h           # CLI commandlet header
    │   ├── BlueprintAuditSubsystem.h            # Editor subsystem header
    │   ├── FathomHttpServer.h                   # HTTP server (asset queries, live coding)
    │   └── AssetRefSubsystem.h                  # Editor subsystem that owns the HTTP server
    └── Private/
        ├── FathomUELinkModule.cpp               # Module startup/shutdown
        ├── BlueprintAuditor.cpp                 # Markdown serialization of Blueprint internals
        ├── BlueprintAuditCommandlet.cpp         # Headless batch audit entry point
        ├── BlueprintAuditSubsystem.cpp          # On-save hooks + startup stale check
        ├── FathomHttpServer.cpp                 # Server infra: Start/Stop/TryBind, marker file
        ├── FathomHttpServerAssetRef.cpp         # Asset ref handlers: deps, refs, search, show
        ├── FathomHttpServerLiveCoding.cpp       # Live Coding handlers: status, compile
        └── AssetRefSubsystem.cpp                # Subsystem lifecycle (start/stop server)
```

## Core Files

- **`BlueprintAuditor.cpp`**: The heart of the plugin. Given a `UBlueprint*`, extracts variables, components, event graphs, function calls, widget trees, property overrides, and interfaces into Markdown. Also computes a file hash (MD5 of the `.uasset`) for staleness detection.
- **`BlueprintAuditCommandlet.cpp`**: CLI entry point (`-run=BlueprintAudit`). Supports two modes: audit a single asset (`-AssetPath=...`) or audit all `/Game/` Blueprints. Designed for headless CI runs and for the Rider plugin to trigger remotely.
- **`BlueprintAuditSubsystem.cpp`**: `UEditorSubsystem` that hooks `PackageSavedWithContextEvent` for automatic re-audit on save. Also runs a deferred stale check on editor startup.
- **`FathomHttpServer.cpp`** + **`FathomHttpServerAssetRef.cpp`** + **`FathomHttpServerLiveCoding.cpp`**: HTTP server (ports 19900-19910) using UE's `FHttpServerModule`. Split by feature: server infrastructure, asset ref handlers (search, show, dependencies, referencers), and Live Coding handlers (status, compile with log capture).
- **`AssetRefSubsystem.cpp`**: `UEditorSubsystem` that owns the `FFathomHttpServer` lifecycle.

## Module Dependencies

From `FathomUELink.Build.cs`:

**Public:**
- Core, CoreUObject, Engine, EditorSubsystem, HTTPServer

**Private:**
- AssetRegistry, BlueprintGraph, Json, UMG, UMGEditor
- LiveCoding (Win64 only)

## Important Notes

- **Windows-only** currently due to hardcoded paths (`Win64`, `UnrealEditor-Cmd.exe`).
- **Editor-only module**: `Type: Editor` in the `.uplugin`, so it doesn't ship in packaged builds.
- **Symlinks on Windows**: Prefer `New-Item -ItemType Junction` over `mklink /D` for directory symlinks. Junctions don't require admin or Developer Mode, and `mklink` is a `cmd.exe` built-in that doesn't work directly in PowerShell.
- **Port binding**: `FFathomHttpServer::TryBind` uses UE's `FHttpServerModule` (raw TCP sockets). This does not detect ports already claimed by Windows HTTP.sys listeners (used by .NET `HttpListener`, e.g. the Rider plugin). Both servers can silently bind to the same port with requests routed unpredictably. The port range (19900-19910) is well separated from Rider's default (19876), but a proper fix would add a TCP probe (attempt a raw `FSocket` connect to `localhost:port`) before calling `GetHttpRouter`, to detect any listener regardless of binding mechanism.

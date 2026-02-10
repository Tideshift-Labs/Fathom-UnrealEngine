# Blueprint Audit: Technical Design

This document explains how the Blueprint audit feature works, why we took this approach, and what limitations to keep in mind.

## The problem

Rider can inspect C++ source code and walk Blueprint derivation trees via reflection on `UE4AssetsCache`, but it has no access to Blueprint internals. `.uasset` is a binary format that only Unreal Engine itself can deserialize. Variables, graphs, nodes, CDO overrides, widget trees, timelines, and interfaces are all invisible from the Rider side.

We needed a way to expose this data for external consumers (LLMs, diffing tools, CI pipelines) without modifying the engine.

## The approach: filesystem contract

The system uses a **filesystem contract** between two independent plugins. No IPC, no sockets, no shared memory, no compile-time dependencies.

```
UE Editor Plugin                          Rider Plugin (.NET backend)
====================                      ===========================

 BlueprintAuditSubsystem (on-save)        BlueprintAuditService
 BlueprintAuditCommandlet (headless)      BlueprintAuditHandler (HTTP)
 BlueprintAuditor (core extraction)       AuditMarkdownFormatter
         |                                          |
         +---writes--->  Saved/Audit/v<N>/  <---reads---+
                         Blueprints/*.json
```

The UE plugin writes JSON files. The Rider plugin reads them. That is the entire interface.

### Why not sockets or IPC?

1. **Process independence.** The UE editor and Rider are separate processes with separate lifecycles. The editor may not be running when Rider needs audit data. The commandlet runs headless without the editor at all.
2. **Simplicity.** File I/O is the most straightforward cross-process, cross-language communication possible. No serialization protocol negotiation, no connection management, no reconnection logic.
3. **Debuggability.** The JSON files are human-readable. You can `cat` them, diff them, grep them. When something goes wrong you can inspect the output directly.
4. **CI compatibility.** The commandlet can run in a headless build pipeline and produce the same output. No server process needed.

## Two-phase architecture (game thread / background thread)

Blueprint data extraction requires access to `UObject` pointers, which are only safe to read on the game thread. But file I/O, hashing, and JSON serialization should not block the editor UI.

The solution is a two-phase design:

### Phase 1: `GatherBlueprintData()` (game thread)

Reads `UBlueprint*` and populates plain-old-data (POD) structs (`FBlueprintAuditData`, `FGraphAuditData`, etc.) that contain no UObject pointers. This is fast since it only copies data out of the engine's in-memory representation.

Extracted data includes:
- Metadata (name, path, parent class, blueprint type)
- Variables with types (including container types like `Array<>`, `Map<>`, `Set<>`), categories, `InstanceEditable`, `Replicated` flags
- Property overrides (CDO diff against parent class defaults)
- Implemented interfaces
- Components from `SimpleConstructionScript`
- Timelines with track counts
- Event graphs, function graphs, macro graphs with full node-level detail
- Widget tree hierarchy (for Widget Blueprints)

### Phase 2: `SerializeToJson()` + `WriteAuditJson()` (background thread)

Takes the POD structs and converts them to `FJsonObject`, computes the `SourceFileHash` (MD5 of the `.uasset` file), and writes to disk. This runs on the thread pool via `Async(EAsyncExecution::ThreadPool, ...)`.

The separation matters because:
- The game thread is never blocked by disk I/O or MD5 computation
- The POD structs are safe to move across thread boundaries (no raw UObject pointers)
- Multiple writes can be in-flight concurrently

## Three execution modes

### 1. On-save subsystem (`UBlueprintAuditSubsystem`)

A `UEditorSubsystem` that hooks `UPackage::PackageSavedWithContextEvent`. When a user saves a Blueprint in the editor, it immediately gathers data on the game thread and dispatches a background write. This keeps audit data fresh during normal editing.

It also hooks `OnAssetRemoved` and `OnAssetRenamed` to delete stale audit JSONs when Blueprints are deleted or moved.

**In-flight dedup:** If a Blueprint is saved while a previous write for the same package is still pending, the second save is skipped. This prevents duplicate writes from rapid save-spam. Tracked via `InFlightPackages` (a `TSet<FString>` guarded by `FCriticalSection`).

### 2. Batch commandlet (`UBlueprintAuditCommandlet`)

Headless, single-run via `UnrealEditor-Cmd.exe -run=BlueprintAudit`. Two modes:
- **Single asset:** `-AssetPath=/Game/UI/WBP_Foo -Output=out.json`
- **All project assets:** Dumps every `/Game/` Blueprint to individual JSON files

Uses the legacy synchronous `AuditBlueprint()` API (which wraps `GatherBlueprintData` + `SerializeToJson` in sequence) since the commandlet runs single-threaded. Collects garbage every 50 assets to manage memory with large projects.

This mode is invoked by the Rider plugin (via `CompanionPluginService`) and is suitable for CI pipelines.

### 3. Startup stale check (subsystem state machine)

On editor startup, the subsystem runs a five-phase state machine that detects and re-audits stale Blueprints:

| Phase | Name | Thread | Purpose |
|-------|------|--------|---------|
| 1 | WaitingForRegistry | Game (tick) | Waits for AssetRegistry to finish loading |
| 2 | BuildingList | Game (tick) | Queries all `/Game/` Blueprints, collects package names and file paths |
| 3 | BackgroundHash | Thread pool | Computes MD5 hashes of `.uasset` files, compares against stored hashes in audit JSONs |
| 4 | ProcessingStale | Game (tick) | Loads stale Blueprints and re-audits them in batches of 5 per tick |
| 5 | Done | Game (tick) | Sweeps orphaned audit files, unregisters ticker |

The key design constraint is **never freezing the editor**. Phase 3 runs entirely on the thread pool. Phase 4 processes only 5 Blueprints per tick, then yields back to the engine. The state machine is driven by `FTSTicker`, which fires once per frame.

After processing completes, `SweepOrphanedAuditFiles()` walks the audit directory and deletes JSON files whose source `.uasset` no longer exists in the AssetRegistry.

## Staleness detection

Staleness is detected by comparing MD5 hashes of `.uasset` files.

Each audit JSON includes a `SourceFileHash` field containing the MD5 hash of the source `.uasset` at the time the audit was generated. To check freshness, compute the current MD5 of the `.uasset` and compare:

```
Stored hash (in JSON)  !=  Current hash (computed from .uasset)  =>  STALE
```

This approach was chosen over timestamps because:
- File modification timestamps can be unreliable across `git checkout`, file copies, and build systems
- Content hashing is the source of truth: if the binary content hasn't changed, the audit is still valid
- Hash comparison is unambiguous (no clock skew, no timezone issues)

### Staleness is checked on both sides

- **UE plugin (subsystem):** On editor startup, background-hashes all `.uasset` files against stored hashes. Re-audits stale entries.
- **Rider plugin (BlueprintAuditService):** On each `GET /blueprint-audit` request, computes current hashes and compares. Returns HTTP 409 if any entries are stale. Also runs a boot check (delayed 5s after solution open) that auto-triggers a commandlet refresh if stale data is detected.

Both sides compute MD5 independently. The UE plugin uses `FMD5Hash::HashFile()`. The Rider plugin uses `System.Security.Cryptography.MD5`. The hex output format matches (lowercase, no separators).

## Schema versioning

The audit JSON schema is versioned via `FBlueprintAuditor::AuditSchemaVersion` (C++) and `BlueprintAuditService.AuditSchemaVersion` (C#). The version is embedded in the output path:

```
Saved/Audit/v2/Blueprints/UI/Widgets/WBP_Foo.json
             ^^
             schema version
```

When the schema changes, bump the version on both sides. All existing cached JSON is automatically invalidated because no files exist at the new versioned path. The Rider plugin's boot check will detect the missing audit directory and trigger a full refresh.

**Cross-repo coordination required:** The C++ constant and the C# constant must always match. If they diverge, the Rider plugin will look for audit files in the wrong directory.

**TODO:** Old `Saved/Audit/v<old>/` directories are not automatically cleaned up. This is a minor disk hygiene issue but not a correctness problem.

## What the JSON captures (and doesn't)

### Captured

| Section | Detail level |
|---------|-------------|
| Variables | Name, type (with container generics), category, `InstanceEditable`, `Replicated` |
| Property overrides | CDO diff: properties where the Blueprint's default differs from the parent class default |
| Components | Name and class from `SimpleConstructionScript` |
| Timelines | Name, length, loop/autoplay flags, track counts per type |
| Event graphs | Per-node: events, function calls (with target class, native flag, hardcoded input values), variable reads/writes, macro instances |
| Function graphs | Same detail as event graphs |
| Macro graphs | Name and node count only |
| Widget tree | Recursive hierarchy with name, class, `IsVariable` flag |
| Interfaces | List of implemented interface names |

### Not captured

- **Node-level spatial layout** (X/Y positions). This is deliberately omitted because it is irrelevant for semantic analysis and would massively bloat the JSON.
- **Pin connections between nodes.** The graph structure (which node connects to which) is not captured. We capture what functions are called and what variables are accessed, but not the execution flow wiring. This is a significant gap for understanding complex graph logic, but was deferred due to the complexity of serializing the full graph topology.
- **Animation Blueprint internals.** `UAnimBlueprint` state machines, blend spaces, and anim graph nodes are not extracted. The auditor handles the base `UBlueprint` class and the `UWidgetBlueprint` subclass, but not `UAnimBlueprint`.
- **Data-only Blueprints.** Blueprints with no graphs (pure data containers) are still audited but produce minimal output (just metadata, variables, and property overrides). This is correct behavior, not a limitation.
- **Comment nodes and reroute nodes.** These are filtered out implicitly because they don't match any of the `UK2Node_*` cast checks.

## Rider-side integration

The Rider plugin (.NET backend) provides HTTP endpoints for accessing audit data:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/blueprint-audit` | GET | Returns audit data. 200 if fresh, 409 if stale, 503 if not ready, 501 if commandlet missing |
| `/blueprint-audit/refresh` | POST | Triggers commandlet (202 accepted, or already in progress) |
| `/blueprint-audit/status` | GET | Refresh progress, boot check status, last output/error |
| `/bp?file=/Game/Path` | GET | Composite: audit data + asset dependencies + referencers |

### Commandlet detection

When the Rider plugin triggers a refresh and the commandlet fails with output containing "unknown commandlet" or similar, it sets a `_commandletMissing` flag. Subsequent requests return HTTP 501 with installation instructions rather than repeatedly attempting to run the commandlet.

### Simple JSON parser

The Rider-side `ParseSimpleJson()` uses regex to extract top-level string, number, and boolean fields rather than performing full JSON deserialization. This is intentional:

- The audit JSON can be large (hundreds of KB for complex Blueprints with many graphs)
- The Rider plugin only needs top-level fields for staleness checks (`SourceFileHash`, `Name`, `Path`)
- Regex extraction of flat key-value pairs is faster and simpler than walking a full JSON DOM
- The full JSON content is passed through as-is to HTTP consumers

This parser deliberately does not handle arrays or nested objects. It only captures the first occurrence of each key at the document level.

## Limitations and known issues

### 1. Full project scan on every commandlet invocation

The batch commandlet (`-run=BlueprintAudit` without `-AssetPath`) re-audits every `/Game/` Blueprint in the project. There is no incremental mode for the commandlet. The subsystem handles incremental updates via on-save hooks, but when Rider triggers a refresh (e.g., after detecting stale data on boot), it runs a full scan.

For large projects with hundreds of Blueprints, this can take tens of seconds. The commandlet uses `CollectGarbage` every 50 assets to manage memory, but the wall-clock time is proportional to Blueprint count.

### 2. Windows-only paths

The build configuration uses hardcoded `Win64` for the platform binary folder and `UnrealEditor-Cmd.exe` for the commandlet executable. Cross-platform support (Mac/Linux) would require platform detection and path mapping.

### 3. Port binding collision potential

The Rider plugin's `HttpListener` (using Windows HTTP.sys) and UE's `FHttpServerModule` (raw TCP sockets) use different binding mechanisms. They can silently bind to the same port with requests routed unpredictably. The port ranges are separated (Rider: 19876, UE: 19900-19910) but there is no active collision detection. A proper fix would add a raw TCP socket probe before binding.

### 4. No old schema version cleanup

When `AuditSchemaVersion` is bumped, old `Saved/Audit/v<old>/` directories are left on disk. They are harmless (the new version simply ignores them) but waste disk space over time.

### 5. Subsystem requires editor

The `UBlueprintAuditSubsystem` is a `UEditorSubsystem` and only runs when the UE editor is open. On-save hooks and the startup stale check do not function when using the commandlet in headless mode. The commandlet handles its own full-project scan independently.

### 6. Race between subsystem and commandlet

If the editor's subsystem is re-auditing a Blueprint (via on-save) at the same time the Rider plugin triggers a commandlet refresh, both may write to the same JSON file concurrently. In practice this is safe because:
- The commandlet runs in a separate process (so no in-process race)
- File writes are atomic at the OS level for reasonable file sizes
- The last writer wins, and both produce correct content

However, the subsystem's in-flight dedup (`InFlightPackages`) only prevents duplicate writes within the editor process. It does not coordinate with the external commandlet process.

### 7. GC pressure during stale check

Phase 4 of the startup stale check loads Blueprint assets via `LoadObject<UBlueprint>` to re-audit them. Each load brings the Blueprint (and potentially its dependencies) into memory. Garbage collection runs every 50 assets (`GCInterval`), but for projects with many stale Blueprints at startup, this can cause temporary memory spikes.

### 8. AssetRegistry race on startup

The stale check waits for `AssetRegistry.IsLoadingAssets()` to return false before querying. In some edge cases (very large projects, slow disks), the registry may report "done" before all assets are actually discoverable. This could cause the stale check to miss some Blueprints on the first pass. Subsequent on-save hooks will catch them as they are edited.

## Cross-repo coordination checklist

When modifying the audit system, keep these in sync between the UE plugin and the Rider plugin:

| Item | UE plugin location | Rider plugin location |
|------|-------------------|----------------------|
| Schema version | `BlueprintAuditor.h` line 116: `AuditSchemaVersion` | `BlueprintAuditService.cs` line 21: `AuditSchemaVersion` |
| Audit output path pattern | `GetAuditBaseDir()`: `Saved/Audit/v<N>/Blueprints/` | `BlueprintAuditService.cs` line 70, 202: hardcoded path construction |
| Commandlet name | `BlueprintAuditCommandlet.h` class name -> `BlueprintAudit` | `BlueprintAuditService.cs` line 293: `-run=BlueprintAudit` |
| JSON field names | `BlueprintAuditor.cpp` `SerializeToJson()` | `BlueprintAuditService.cs` `ParseSimpleJson()` and `ReadAndCheckBlueprintAudit()` |
| Hash algorithm | `FMD5Hash::HashFile()` (MD5, lowercase hex) | `MD5.Create()` + `BitConverter.ToString().Replace("-","").ToLowerInvariant()` |

## Testing

### Commandlet

```powershell
# Audit all Blueprints
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -unattended -nopause

# Audit single Blueprint
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -AssetPath=/Game/UI/WBP_MainMenu
```

Verify output at `<ProjectDir>/Saved/Audit/v<N>/Blueprints/`.

### On-save hooks

1. Open the UE project in the editor with the plugin installed
2. Open and save a Blueprint
3. Check that the corresponding JSON file in `Saved/Audit/v<N>/Blueprints/` was updated
4. Verify `SourceFileHash` in the JSON matches the current `.uasset` MD5

### Rider integration

```bash
curl http://localhost:19876/blueprint-audit/status
curl http://localhost:19876/blueprint-audit?format=json
curl -X POST http://localhost:19876/blueprint-audit/refresh
```

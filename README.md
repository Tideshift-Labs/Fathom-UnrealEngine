# CoRider-UnrealEngine

An Unreal Engine editor plugin that exports Blueprint asset summaries to Markdown for external analysis, diffing, and LLM integration.

## Features

- **Markdown Export**: Extracts comprehensive Blueprint metadata including variables, components, event graphs, function calls, and widget trees into a token-efficient Markdown format
- **Commandlet Support**: Run audits from command line without opening the editor UI
- **On-Save Hooks**: Automatically re-audit Blueprints when saved (via editor subsystem)
- **Staleness Detection**: Includes source file hashes for detecting when audit data is out of date
- **Widget Blueprint Support**: Extracts widget hierarchies from UMG Widget Blueprints

## Installation

### Option 1: Symlink (Development)

Create a symbolic link from your project's Plugins folder:

```powershell
# From your UE project directory (Junction doesn't require admin or Developer Mode)
New-Item -ItemType Junction -Path "Plugins\CoRiderUnrealEngine" -Target "path\to\CoRider-UnrealEngine"
```

### Option 2: Copy

Copy the `CoRiderUnrealEngine` folder to your project's `Plugins` directory.

## Usage

### Commandlet (Recommended for CI/Automation)

Audit all Blueprints in the project:

```bash
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -unattended -nopause
```

Audit a single Blueprint:

```bash
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -AssetPath=/Game/UI/WBP_MainMenu -Output=audit.md
```

### Output Location

- **All Blueprints**: `<ProjectDir>/Saved/Audit/v<N>/Blueprints/<relative_path>.md`
- **Single Blueprint**: Specified via `-Output` or defaults to `<ProjectDir>/BlueprintAudit.md`

The `v<N>` segment is the audit schema version (`FBlueprintAuditor::AuditSchemaVersion`). When the version is bumped, all cached files are automatically invalidated because no files exist at the new path.

> **TODO:** Add automatic cleanup of old `Saved/Audit/v<old>/` directories on startup.

### On-Save (Automatic)

When the editor is running, the `UBlueprintAuditSubsystem` automatically re-audits Blueprints when they are saved.

## Markdown Output Format

The audit output is Markdown, optimized for LLM consumption (more token-efficient than JSON). Sections with no data are omitted entirely.

````markdown
# WBP_MainMenu
Path: /Game/UI/WBP_MainMenu.WBP_MainMenu
Parent: /Script/CommonUI.CommonActivatableWidget
Type: Normal
Hash: fe020519d8ca4cf5b2e8690bd0bfabca

## Variables
| Name | Type | Category | Editable | Replicated |
|------|------|----------|----------|------------|
| PlayerName | String | Default | Yes | No |

## Property Overrides
- bAutoActivate = True

## Interfaces
- IMenuInterface

## Components
| Name | Class |
|------|-------|
| RootComponent | SceneComponent |

## Timelines
| Name | Length | Loop | AutoPlay | Float | Vector | Color | Event |
|------|--------|------|----------|-------|--------|-------|-------|
| FadeTimeline | 2.00 | No | No | 1 | 0 | 0 | 0 |

## Widget Tree
- CanvasPanel_0 (CanvasPanel)
  - Button_Start (Button) [var]
  - Text_Title (TextBlock)
  - WBP_TemplateLayout (WBP_TemplateLayout_C)
    - VerticalBox_Content (VerticalBox) [slot:ContentSlot]

## EventGraph
| Id | Type | Name | Details |
|----|------|------|---------|
| 0 | Event | Event BeginPlay | |
| 1 | CallFunction | IsValid | KismetSystemLibrary, pure |
| 2 | Branch | Branch | |
| 3 | CallFunction | PlayAnimation | UserWidget, not-native |
| 4 | VariableGet | PlayerName | pure |
| 5 | VariableSet | bIsActive | |

Exec: 0->2, 2-[True]->3, 2-[False]->5
Data: 4.PlayerName->1.Object, 1.ReturnValue->2.Condition

## Function: GetFormattedName(Prefix: String) -> ReturnValue: String
| Id | Type | Name | Details |
|----|------|------|---------|
| 0 | FunctionEntry | GetFormattedName | |
| 1 | CallFunction | Concat_StrStr | KismetStringLibrary, pure |
| 2 | VariableGet | PlayerName | pure |
| 3 | FunctionResult | Return | |

Exec: 0->3
Data: 0.Prefix->1.A, 2.PlayerName->1.B, 1.ReturnValue->3.ReturnValue
````

### Format Details

**Header lines**: Name (H1 heading), Path, Parent, Type, Hash. Used for staleness detection and quick identification.

**Node tables** use `| Id | Type | Name | Details |` columns. The Details column contains target class, flags (pure, latent, not-native), and hardcoded default input values.

**Edge one-liners**: Compact notation after each node table.
- Exec edges: `SrcId-[PinName]->DstId`. Pin name omitted when it is "then": `0->1`.
- Data edges: `SrcId.PinName->DstId.PinName`.

**Graph headings**:
- Event graphs: `## EventGraph` (or the graph name)
- Functions: `## Function: Name(params) -> returns`
- Macros: `## Macro: Name`

**Widget tree**: Indented list with `[var]` suffix for variable widgets and `[slot:Name]` suffix for content placed in named slots of template widgets.

**Node types**: `FunctionEntry`, `FunctionResult`, `Event`, `CustomEvent`, `CallFunction`, `Branch`, `Sequence`, `VariableGet`, `VariableSet`, `MacroInstance`, `Timeline`, `Other`.

Reroute/knot nodes are skipped; edges trace through them to the real endpoints.

## HTTP API (Asset Reference Server)

When the editor is running, the plugin starts a lightweight HTTP server (ports 19900-19910) for asset queries.

| Endpoint | Description |
|----------|-------------|
| `GET /asset-refs/health` | Server status |
| `GET /asset-refs/dependencies?asset=/Game/Path` | Asset dependencies |
| `GET /asset-refs/referencers?asset=/Game/Path` | Asset referencers |
| `GET /asset-refs/search?q=term` | Fuzzy search for assets by name |

### Asset Search Parameters

| Param | Required | Description |
|-------|----------|-------------|
| `q` | Yes | Search terms (space-separated, all must match). Matched against asset name and package path. |
| `class` | No | Filter by asset class (e.g. `WidgetBlueprint`, `DataTable`) |
| `pathPrefix` | No | Filter by package path prefix (e.g. `/Game` for project assets only) |
| `limit` | No | Max results to return (default: 50) |

Multi-word queries match each token independently (e.g. `q=main menu` finds assets containing both "main" and "menu" in any order). Scoring per token: exact name match > name prefix > name substring > path-only match. The final score is the minimum across all tokens.

```powershell
# Multi-word search
curl "http://localhost:19900/asset-refs/search?q=main+menu&pathPrefix=/Game"

# Filter by class
curl "http://localhost:19900/asset-refs/search?q=widget&class=WidgetBlueprint&limit=5"
```

## Integration with Rider Plugin

This plugin is designed to work with the companion Rider plugin (`CoRider`). The Rider plugin:

1. Detects when audit data is stale by comparing the `Hash` header field with current file hashes
2. Automatically triggers the commandlet to refresh stale data
3. Exposes audit data via HTTP endpoints for LLM integration

## Requirements

- Unreal Engine 5.x (tested with 5.7)
- Editor builds only (not packaged games)

## Architecture

```
CoRider-UnrealEngine/
└── Source/CoRiderUnrealEngine/
    ├── CoRiderUnrealEngine.Build.cs           # Module build rules
    ├── Public/
    │   ├── CoRiderUnrealEngineModule.h        # Module interface
    │   ├── BlueprintAuditor.h                 # Core audit logic + AuditSchemaVersion
    │   ├── BlueprintAuditCommandlet.h         # CLI commandlet header
    │   └── BlueprintAuditSubsystem.h          # Editor subsystem header
    └── Private/
        ├── CoRiderUnrealEngineModule.cpp      # Module startup/shutdown
        ├── BlueprintAuditor.cpp               # Markdown serialization of Blueprint internals
        ├── BlueprintAuditCommandlet.cpp        # Headless batch audit entry point
        └── BlueprintAuditSubsystem.cpp         # On-save hooks + startup stale check
```

### Core Files

- **`BlueprintAuditor.cpp`**: The heart of the plugin. Given a `UBlueprint*`, extracts variables, components, event graphs, function calls, widget trees, property overrides, and interfaces into Markdown. Also computes a file hash (MD5 of the `.uasset`) for staleness detection.
- **`BlueprintAuditCommandlet.cpp`**: CLI entry point (`-run=BlueprintAudit`). Supports two modes: audit a single asset (`-AssetPath=...`) or audit all `/Game/` Blueprints. Designed for headless CI runs and for the Rider plugin to trigger remotely.
- **`BlueprintAuditSubsystem.cpp`**: `UEditorSubsystem` that hooks `PackageSavedWithContextEvent` for automatic re-audit on save. Also runs a deferred stale check on editor startup.

## Development Workflow

### Setup

1. Symlink or copy into a UE project's `Plugins/` directory:
   ```powershell
   # From the UE project directory; use Junction (no admin required)
   New-Item -ItemType Junction -Path "Plugins\CoRiderUnrealEngine" -Target "D:\path\to\CoRider-UnrealEngine"
   ```
2. Regenerate project files and build the UE project as normal.

### Testing the commandlet

```powershell
# Audit all Blueprints
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -unattended -nopause

# Audit a single Blueprint
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -AssetPath=/Game/UI/WBP_MainMenu
```

Verify output at `<ProjectDir>/Saved/Audit/v<N>/Blueprints/`.

### Testing on-save hooks

1. Open the UE project in the editor (with the plugin installed).
2. Open and save a Blueprint asset.
3. Check that the corresponding `.md` file in `Saved/Audit/v<N>/Blueprints/` was updated.

### Testing with the Rider plugin

1. Ensure this plugin is installed in the UE project.
2. Open the UE project in Rider (with the CoRider plugin).
3. Check audit status: `curl http://localhost:19876/blueprint-audit/status`
4. If stale: `curl http://localhost:19876/blueprint-audit/refresh`

## Cross-Repo Coordination

This plugin works with the companion [CoRider](https://github.com/kvirani/CoRider) Rider plugin. When modifying the audit JSON schema, keep these in sync:

- **Audit schema version**: `FBlueprintAuditor::AuditSchemaVersion` in `BlueprintAuditor.h` (this repo) must match `BlueprintAuditService.AuditSchemaVersion` in the Rider repo. Bump both together when the audit format changes.
- **Audit output path**: `Saved/Audit/v<N>/Blueprints/...`. The `v<N>` version segment invalidates cached files automatically. Both sides must agree on this path structure.
- **Commandlet name**: `BlueprintAudit`, hardcoded on both sides. The Rider plugin invokes `UnrealEditor-Cmd.exe -run=BlueprintAudit`.

## Important Notes

- **Windows-only** currently due to hardcoded paths (`Win64`, `UnrealEditor-Cmd.exe`).
- **Editor-only module**: `Type: Editor` in the `.uplugin`, so it doesn't ship in packaged builds.
- **Symlinks on Windows**: Prefer `New-Item -ItemType Junction` over `mklink /D` for directory symlinks. Junctions don't require admin or Developer Mode, and `mklink` is a `cmd.exe` built-in that doesn't work directly in PowerShell.
- **Port binding**: `FAssetRefHttpServer::TryBind` uses UE's `FHttpServerModule` (raw TCP sockets). This does not detect ports already claimed by Windows HTTP.sys listeners (used by .NET `HttpListener`, e.g. the Rider plugin). Both servers can silently bind to the same port with requests routed unpredictably. The port range (19900-19910) is well separated from Rider's default (19876), but a proper fix would add a TCP probe (attempt a raw `FSocket` connect to `localhost:port`) before calling `GetHttpRouter`, to detect any listener regardless of binding mechanism.

## Module Dependencies

- Core, CoreUObject, Engine
- AssetRegistry, BlueprintGraph, UnrealEd
- Json (private, used by AssetRefHttpServer)
- Slate, SlateCore
- UMG, UMGEditor

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

# CoRider-UnrealEngine

An Unreal Engine editor plugin that exports Blueprint asset summaries to JSON for external analysis, diffing, and LLM integration.

## Features

- **JSON Export**: Extracts comprehensive Blueprint metadata including variables, components, event graphs, function calls, and widget trees
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
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -AssetPath=/Game/UI/WBP_MainMenu -Output=audit.json
```

### Output Location

- **All Blueprints**: `<ProjectDir>/Saved/Audit/v<N>/Blueprints/<relative_path>.json`
- **Single Blueprint**: Specified via `-Output` or defaults to `<ProjectDir>/BlueprintAudit.json`

The `v<N>` segment is the audit schema version (`FBlueprintAuditor::AuditSchemaVersion`). When the version is bumped, all cached JSON is automatically invalidated because no files exist at the new path.

> **TODO:** Add automatic cleanup of old `Saved/Audit/v<old>/` directories on startup.

### On-Save (Automatic)

When the editor is running, the `UBlueprintAuditSubsystem` automatically re-audits Blueprints when they are saved.

## JSON Output Schema

```json
{
  "Name": "WBP_MainMenu",
  "Path": "/Game/UI/WBP_MainMenu.WBP_MainMenu",
  "ParentClass": "/Script/CommonUI.CommonActivatableWidget",
  "BlueprintType": "Normal",
  "SourceFileHash": "a1b2c3d4e5f6...",

  "Variables": [
    {
      "Name": "PlayerName",
      "Type": "String",
      "Category": "Default",
      "InstanceEditable": true,
      "Replicated": false
    }
  ],

  "PropertyOverrides": [
    {"Name": "bAutoActivate", "Value": "True"}
  ],

  "Interfaces": ["IMenuInterface"],

  "Components": [
    {"Name": "RootComponent", "Class": "SceneComponent"}
  ],

  "Timelines": [
    {
      "Name": "FadeTimeline",
      "Length": 2.0,
      "Looping": false,
      "AutoPlay": false,
      "FloatTrackCount": 1,
      "VectorTrackCount": 0,
      "LinearColorTrackCount": 0,
      "EventTrackCount": 0
    }
  ],

  "WidgetTree": {
    "Name": "CanvasPanel_0",
    "Class": "CanvasPanel",
    "IsVariable": false,
    "Children": [...]
  },

  "EventGraphs": [
    {
      "Name": "EventGraph",
      "TotalNodes": 42,
      "Events": ["Event BeginPlay", "CustomEvent: OnMenuOpened"],
      "FunctionCalls": [
        {
          "Function": "PlayAnimation",
          "Target": "UserWidget",
          "IsNative": true,
          "DefaultInputs": [
            {"Name": "PlaybackSpeed", "Value": "1.5"}
          ]
        }
      ],
      "VariablesRead": ["PlayerName"],
      "VariablesWritten": ["bIsActive"],
      "MacroInstances": ["IsValid"]
    }
  ],

  "FunctionGraphs": [...],
  "MacroGraphs": [...]
}
```

## Integration with Rider Plugin

This plugin is designed to work with the companion Rider plugin (`CoRider`). The Rider plugin:

1. Detects when audit data is stale by comparing `SourceFileHash` with current file hashes
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
        ├── BlueprintAuditor.cpp               # JSON serialization of Blueprint internals
        ├── BlueprintAuditCommandlet.cpp        # Headless batch audit entry point
        └── BlueprintAuditSubsystem.cpp         # On-save hooks + startup stale check
```

### Core Files

- **`BlueprintAuditor.cpp`**: The heart of the plugin. Given a `UBlueprint*`, extracts variables, components, event graphs, function calls, widget trees, property overrides, and interfaces into a JSON object. Also computes `SourceFileHash` (MD5 of the `.uasset`) for staleness detection.
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
3. Check that the corresponding JSON file in `Saved/Audit/v<N>/Blueprints/` was updated.

### Testing with the Rider plugin

1. Ensure this plugin is installed in the UE project.
2. Open the UE project in Rider (with the CoRider plugin).
3. Check audit status: `curl http://localhost:19876/blueprint-audit/status`
4. If stale: `curl http://localhost:19876/blueprint-audit/refresh`

## Cross-Repo Coordination

This plugin works with the companion [CoRider](https://github.com/kvirani/CoRider) Rider plugin. When modifying the audit JSON schema, keep these in sync:

- **Audit schema version**: `FBlueprintAuditor::AuditSchemaVersion` in `BlueprintAuditor.h` (this repo) must match `BlueprintAuditService.AuditSchemaVersion` in the Rider repo. Bump both together when the JSON schema changes.
- **Audit output path**: `Saved/Audit/v<N>/Blueprints/...`. The `v<N>` version segment invalidates cached JSON automatically. Both sides must agree on this path structure.
- **Commandlet name**: `BlueprintAudit`, hardcoded on both sides. The Rider plugin invokes `UnrealEditor-Cmd.exe -run=BlueprintAudit`.

## Important Notes

- **Windows-only** currently due to hardcoded paths (`Win64`, `UnrealEditor-Cmd.exe`).
- **Editor-only module**: `Type: Editor` in the `.uplugin`, so it doesn't ship in packaged builds.
- **Symlinks on Windows**: Prefer `New-Item -ItemType Junction` over `mklink /D` for directory symlinks. Junctions don't require admin or Developer Mode, and `mklink` is a `cmd.exe` built-in that doesn't work directly in PowerShell.
- **Port binding**: `FAssetRefHttpServer::TryBind` uses UE's `FHttpServerModule` (raw TCP sockets). This does not detect ports already claimed by Windows HTTP.sys listeners (used by .NET `HttpListener`, e.g. the Rider plugin). Both servers can silently bind to the same port with requests routed unpredictably. The port range (19900-19910) is well separated from Rider's default (19876), but a proper fix would add a TCP probe (attempt a raw `FSocket` connect to `localhost:port`) before calling `GetHttpRouter`, to detect any listener regardless of binding mechanism.

## Module Dependencies

- Core, CoreUObject, Engine
- AssetRegistry, BlueprintGraph, UnrealEd
- Json
- Slate, SlateCore
- UMG, UMGEditor

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

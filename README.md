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
# From your UE project directory
New-Item -ItemType SymbolicLink -Path "Plugins\CoRiderUnrealEngine" -Target "path\to\CoRiderUnrealEngine"
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

- **All Blueprints**: `<ProjectDir>/Saved/Audit/Blueprints/<relative_path>.json`
- **Single Blueprint**: Specified via `-Output` or defaults to `<ProjectDir>/BlueprintAudit.json`

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
        {"Function": "PlayAnimation", "Target": "UserWidget"}
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

## Module Dependencies

- Core, CoreUObject, Engine
- AssetRegistry, BlueprintGraph, UnrealEd
- Json
- Slate, SlateCore
- UMG, UMGEditor

## License

[TBD]

# Development Workflow

## Setup

1. Symlink or copy into a UE project's `Plugins/` directory:
   ```powershell
   # From the UE project directory; use Junction (no admin required)
   New-Item -ItemType Junction -Path "Plugins\FathomUELink" -Target "D:\path\to\Fathom-UnrealEngine"
   ```
2. Regenerate project files and build the UE project as normal.

## Testing the Commandlet

```powershell
# Audit all Blueprints
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -unattended -nopause

# Audit a single Blueprint
UnrealEditor-Cmd.exe "path/to/Project.uproject" -run=BlueprintAudit -AssetPath=/Game/UI/WBP_MainMenu
```

Verify output at `<ProjectDir>/Saved/Fathom/Audit/v<N>/Blueprints/`.

## Testing On-Save Hooks

1. Open the UE project in the editor (with the plugin installed).
2. Open and save a Blueprint asset.
3. Check that the corresponding `.md` file in `Saved/Fathom/Audit/v<N>/Blueprints/` was updated.

## Testing with the Rider Plugin

1. Ensure this plugin is installed in the UE project.
2. Open the UE project in Rider (with the Fathom plugin).
3. Check audit status: `curl http://localhost:19876/blueprint-audit/status`
4. If stale: `curl http://localhost:19876/blueprint-audit/refresh`

## Cross-Repo Coordination

This plugin works with the companion [Fathom](https://github.com/Tideshift-Labs/Fathom) Rider plugin. When modifying the audit schema, keep these in sync:

- **Audit schema version**: `FBlueprintAuditor::AuditSchemaVersion` in `BlueprintAuditor.h` (this repo) must match `BlueprintAuditService.AuditSchemaVersion` in the Rider repo. Bump both together when the audit format changes.
- **Audit output path**: `Saved/Fathom/Audit/v<N>/Blueprints/...`. The `v<N>` version segment invalidates cached files automatically. Both sides must agree on this path structure.
- **Commandlet name**: `BlueprintAudit`, hardcoded on both sides. The Rider plugin invokes `UnrealEditor-Cmd.exe -run=BlueprintAudit`.

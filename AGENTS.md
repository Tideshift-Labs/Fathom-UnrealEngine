# Agent Instructions: Fathom-UnrealEngine (UE Plugin)

## Style

- **Never use emdashes** (`—`). Rephrase instead.

## Key Documentation

- **[README.md](README.md)**: Features, installation, usage, JSON schema, architecture, development workflow.

## Build Context

This is an **Unreal Engine editor plugin**, not a standalone project. It must be symlinked or copied into a UE project's `Plugins/` directory or the engine's `Engine/Plugins/Marketplace/` directory to build. It cannot be compiled independently because it depends on the UE build system (`UnrealBuildTool`).

Module type is **Editor-only** (`Type: Editor`) and does not ship in packaged/cooked builds.

## Cross-Repo Coordination

This plugin works with the companion [Fathom](https://github.com/Tideshift-Labs/Fathom) Rider plugin. The contract is purely filesystem conventions, with no IPC, no sockets, and no compile-time dependencies.

- **Audit schema version**: `FBlueprintAuditor::AuditSchemaVersion` in `BlueprintAuditor.h` (this repo) must match `BlueprintAuditService.AuditSchemaVersion` in the Rider repo. **Bump both together** when the JSON schema changes.
- **Audit output path**: `Saved/Fathom/Audit/v<N>/Blueprints/...`. The version segment invalidates cached data automatically.
- **Commandlet name**: `BlueprintAudit` is hardcoded on both sides.

## Staleness Detection

- Each audit JSON includes a `SourceFileHash` field containing the MD5 hash of the source `.uasset` file at audit time.
- The Rider plugin compares this stored hash against the current `.uasset` file hash to detect staleness.
- If stale, the Rider plugin triggers `UnrealEditor-Cmd.exe -run=BlueprintAudit` to regenerate.
- The on-save subsystem (`UBlueprintAuditSubsystem`) re-audits automatically when Blueprints are saved in the editor.

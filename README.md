# Fathom-UnrealEngine

An Unreal Engine editor plugin that exports Blueprint asset summaries to Markdown for external analysis, diffing, and LLM integration.

## Features

- **Markdown Export**: Extracts comprehensive Blueprint metadata including variables, components, event graphs, function calls, and widget trees into a token-efficient Markdown format
- **Commandlet Support**: Run audits from command line without opening the editor UI
- **On-Save Hooks**: Automatically re-audit Blueprints when saved (via editor subsystem)
- **Staleness Detection**: Includes source file hashes for detecting when audit data is out of date
- **Widget Blueprint Support**: Extracts widget hierarchies from UMG Widget Blueprints
- **Asset Reference Server**: HTTP API for querying asset dependencies, referencers, and metadata

## Installation

### Option 1: Symlink (Development)

Create a symbolic link from your project's Plugins folder:

```powershell
# From your UE project directory (Junction doesn't require admin or Developer Mode)
New-Item -ItemType Junction -Path "Plugins\FathomUELink" -Target "path\to\CoRider-UnrealEngine"
```

### Option 2: Copy

Copy the `FathomUELink` plugin folder to your project's `Plugins` directory.

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

### On-Save (Automatic)

When the editor is running, the `UBlueprintAuditSubsystem` automatically re-audits Blueprints when they are saved.

### Audit Output

The audit produces Markdown optimized for LLM consumption (more token-efficient than JSON). Each file includes a header block (name, path, parent class, hash), followed by sections for variables, components, event graphs, functions, widget trees, and more. Sections with no data are omitted. See the [audit format reference](docs/audit_format.md) for the full specification and examples.

## Integration with Rider Plugin

This plugin is designed to work with the companion Rider plugin ([Fathom](https://github.com/Tideshift-Labs/Fathom)). The Rider plugin:

1. Detects when audit data is stale by comparing the `Hash` header field with current file hashes
2. Automatically triggers the commandlet to refresh stale data
3. Exposes audit data via HTTP endpoints for LLM integration
4. Bundles this plugin and auto-installs it into the user's project `Plugins/` directory

### Versioning

This plugin's `VersionName` in `FathomUELink.uplugin` is kept in sync with the Rider plugin's `PluginVersion`. The Rider plugin compares these at runtime to detect outdated installations. Do not bump the version here manually; use the bump script in the Fathom repo instead:

```powershell
# From the Fathom repo root
.\scripts\bump-version.ps1 -Version X.Y.Z
```

See [CoRider/docs/release.md](../CoRider/docs/release.md) for the full release process.

## Requirements

- Unreal Engine 5.x (tested with 5.7)
- Editor builds only (not packaged games)

## Documentation

- **[Audit Format](docs/audit_format.md)**: Full Markdown output specification with examples
- **[HTTP API](docs/http_api.md)**: Asset reference server endpoints, parameters, and scoring
- **[Architecture](docs/architecture.md)**: Source tree, core files, module dependencies, and important notes
- **[Development](docs/development.md)**: Setup, testing, and cross-repo coordination

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

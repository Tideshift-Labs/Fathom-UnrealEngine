# Fathom-UnrealEngine

The Unreal Engine companion plugin for [Fathom](https://github.com/Tideshift-Labs/Fathom). Provides UE5-specific project analysis that Fathom surfaces to AI agents.

Unreal Engine keeps much of its project data in binary formats (`.uasset`) that external tools cannot read. This plugin runs inside the UE editor to extract that data and make it available to Fathom, which in turn exposes it to agents via its MCP server and HTTP API.

**You do not need to install this plugin manually.** The Fathom Rider plugin bundles it and prompts you to install it. You can choose to install to the **Engine** (`Engine/Plugins/Marketplace/`, recommended) or the **Game** project (`Plugins/` directory).

## What it provides

- **Blueprint Audit**: Extracts comprehensive Blueprint metadata (variables, components, graphs, timelines, widget trees, CDO overrides) into token-efficient Markdown files
- **Asset Reference Server**: HTTP API for querying asset dependencies, referencers, fuzzy search, and metadata (ports 19900-19910)
- **On-Save Hooks**: Automatically re-audits Blueprints when saved, and cleans up audit files when assets are deleted or renamed
- **Startup Stale Check**: Background state machine detects and re-audits stale Blueprints on editor launch without freezing the UI
- **Commandlet Support**: Run audits headless from the command line for CI/automation
- **Staleness Detection**: MD5 hashes of `.uasset` files detect when audit data is out of date

## Installation (Development)

For normal use, Fathom installs this plugin automatically. The instructions below are only for developing or debugging the plugin itself.

### Symlink

```powershell
# From your UE project directory (Junction doesn't require admin or Developer Mode)
New-Item -ItemType Junction -Path "Plugins\FathomUELink" -Target "path\to\Fathom-UnrealEngine"
```

### Copy

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

- **All Blueprints**: `<ProjectDir>/Saved/Fathom/Audit/v<N>/<relative_path>.md`
- **Single Blueprint**: Specified via `-Output` or defaults to `<ProjectDir>/BlueprintAudit.md`

The `v<N>` segment is the audit schema version (`FAuditFileUtils::AuditSchemaVersion`). When the version is bumped, all cached files are automatically invalidated because no files exist at the new path.

### On-Save (Automatic)

When the editor is running, the `UBlueprintAuditSubsystem` automatically re-audits Blueprints when they are saved.

### Audit Output

The audit produces Markdown optimized for LLM consumption (more token-efficient than JSON). Each file includes a header block (name, path, parent class, hash), followed by sections for variables, components, event graphs, functions, widget trees, and more. Sections with no data are omitted. See the [audit format reference](docs/audit_format.md) for the full specification and examples.

## Versioning

This plugin's `VersionName` in `FathomUELink.uplugin` is kept in sync with the Rider plugin's `PluginVersion`. The Rider plugin compares these at runtime to detect outdated installations. Do not bump the version here manually; use the bump script in the Fathom repo instead:

```powershell
# From the Fathom repo root
.\scripts\bump-version.ps1 -Version X.Y.Z
```

See [Fathom/docs/release.md](../Fathom/docs/release.md) for the full release process.

## Requirements

- Unreal Engine 5.x (tested with 5.7)
- Editor builds only (not packaged games)

## Troubleshooting

### `BuildPlugin` fails with `MarketplaceRules.dll` being used by another process

When the plugin is installed to the engine (`Engine/Plugins/Marketplace/...`), `BuildPlugin` compiles every marketplace plugin's `*.Build.cs` into a single per-user rules assembly at `%LOCALAPPDATA%\UnrealEngine\Intermediate\Build\BuildRules\MarketplaceRules.dll`. If anything else has that DLL loaded, the build aborts with:

```
IOException: The process cannot access the file '...\MarketplaceRules.dll' because it is being used by another process.
```

Epic Games Launcher being open is not on its own a reliable trigger; many builds succeed with the Launcher running. The lock typically shows up when something is actively touching the marketplace plugin set, for example:

- Epic Games Launcher is verifying, installing, updating, or repairing the engine version (including applying a fix-pack such as 5.7.3 to 5.7.4).
- The Launcher is refreshing its Vault or Installed Plugins view right after a marketplace install or uninstall.
- A second `UnrealBuildTool` invocation is running in parallel (another editor instance, a `Generate Project Files` run, or a concurrent build from Rider).
- A previous build crashed and left a helper process holding the assembly.

Workarounds, in order of preference:

1. Close the contending process (most often Epic Games Launcher), wait for any in-flight engine verify or install to finish, and rerun the build.
2. If the lock persists after closing the Launcher, check Task Manager for stray `UnrealBuildTool`, `MSBuild`, or `dotnet` processes referencing `MarketplaceRules.dll` and end them.
3. As a more permanent workaround, install the plugin to the game project (`<Project>/Plugins/FathomUELink`) instead of the engine. Project-level plugins compile into a per-project rules assembly and do not contend with `MarketplaceRules.dll`.

## Documentation

- **[Audit Format](docs/audit_format.md)**: Full Markdown output specification with examples
- **[HTTP API](docs/http_api.md)**: Asset reference server endpoints, parameters, and scoring
- **[Architecture](docs/architecture.md)**: Source tree, core files, module dependencies, and important notes
- **[Development](docs/development.md)**: Setup, testing, and cross-repo coordination

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

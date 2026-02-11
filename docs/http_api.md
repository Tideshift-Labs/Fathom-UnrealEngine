# HTTP API (Asset Reference Server)

When the editor is running, the plugin starts a lightweight HTTP server (ports 19900-19910) for asset queries.

## Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /asset-refs/health` | Server status |
| `GET /asset-refs/dependencies?asset=/Game/Path` | Asset dependencies |
| `GET /asset-refs/referencers?asset=/Game/Path` | Asset referencers |
| `GET /asset-refs/search?q=term` | Fuzzy search for assets by name |
| `GET /asset-refs/show?package=/Game/Path` | Asset detail: metadata, disk size, tags, dependency/referencer counts |

## Asset Search Parameters

| Param | Required | Description |
|-------|----------|-------------|
| `q` | Yes | Search terms (space-separated, all must match). Matched against asset name and package path. |
| `class` | No | Filter by asset class (e.g. `WidgetBlueprint`, `DataTable`) |
| `pathPrefix` | No | Filter by package path prefix (e.g. `/Game` for project assets only) |
| `limit` | No | Max results to return (default: 50) |

## Asset Show Parameters

| Param | Required | Description |
|-------|----------|-------------|
| `package` | Yes | Full package path (e.g. `/Game/UI/WBP_MainMenu`) |

Returns: `package`, `name`, `assetClass`, `diskPath`, `diskSizeBytes`, `dependencyCount`, `referencerCount`, and `tags` (all registry tag key-value pairs).

## Scoring

Multi-word queries match each token independently (e.g. `q=main menu` finds assets containing both "main" and "menu" in any order). Scoring per token: exact name match > name prefix > name substring > path-only match. The final score is the minimum across all tokens.

## Examples

```powershell
# Multi-word search
curl "http://localhost:19900/asset-refs/search?q=main+menu&pathPrefix=/Game"

# Filter by class
curl "http://localhost:19900/asset-refs/search?q=widget&class=WidgetBlueprint&limit=5"
```

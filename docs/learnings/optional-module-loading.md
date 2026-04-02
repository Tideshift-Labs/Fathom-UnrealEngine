# Optional Module Loading for Plugin Dependencies

When a UE plugin needs to optionally depend on another plugin (e.g., FathomUELink optionally using StateTree), use UE's built-in optional module loading pattern instead of runtime reflection.

## The Problem

FathomUELink is installed at the engine level (shared across projects). A hard `StateTreeModule` dependency in `Build.cs` means:
- Compiles fine when StateTree is enabled
- On a project without StateTree, the DLL references missing symbols and the **entire plugin fails to load**

A separate module in the same `.uplugin` also fails: `TryLoadModulesForPlugin` iterates all modules, and if any module's DLL is missing, it errors and blocks the whole plugin.

## Dead End: Runtime Reflection

Using `FindObject<UClass>` + `FindPropertyByName` to read StateTree data at runtime with zero compile-time coupling. This works but is over-engineered:
- Lots of boilerplate (`FindPropertyByName`, `ContainerPtrToValuePtr`)
- String-based property access is fragile and silent on failure
- UE API changes won't be caught at compile time

## Correct Approach: Optional Module with Dynamic Loading

### 1. Define the optional module in `.uplugin`

Add the module with `"LoadingPhase": "None"` so UE does not auto-load it:

```json
{
  "Modules": [
    {
      "Name": "FathomUELink",
      "Type": "Runtime",
      "LoadingPhase": "Default"
    },
    {
      "Name": "FathomUELinkStateTree",
      "Type": "Runtime",
      "LoadingPhase": "None"
    }
  ]
}
```

### 2. Add the plugin dependency as optional and disabled

```json
{
  "Plugins": [
    {
      "Name": "StateTree",
      "Enabled": false,
      "Optional": true
    }
  ]
}
```

### 3. Dynamically load the module if the plugin is present

In the main module's `StartupModule()`:

```cpp
void FFathomUELinkModule::LoadOptionalModuleIfPluginsPresent(
    const FName& ModuleToLoad,
    const TArray<FName>& RequiredPlugins)
{
    if (!FModuleManager::Get().IsModuleLoaded(ModuleToLoad))
    {
        bool AllPluginsPresent = true;
        for (const FName PluginName : RequiredPlugins)
        {
            if (!FModuleManager::Get().IsModuleLoaded(PluginName))
            {
                AllPluginsPresent = false;
                break;
            }
        }

        if (AllPluginsPresent)
        {
            FModuleManager::Get().LoadModule(ModuleToLoad);
        }
    }
}

void FFathomUELinkModule::LoadOptionalModules()
{
    LoadOptionalModuleIfPluginsPresent(
        "FathomUELinkStateTree", {"StateTree"});
}

void FFathomUELinkModule::StartupModule()
{
    LoadOptionalModules();

    // Also handle late-loading plugins
    FModuleManager::Get().OnModulesChanged().AddLambda(
        [this](FName ModuleName, const EModuleChangeReason ChangeReason)
        {
            if (ChangeReason == EModuleChangeReason::ModuleLoaded)
            {
                LoadOptionalModules();
            }
        });
}
```

## Why This Is Better

| | Optional Module | Runtime Reflection |
|---|---|---|
| Type safety | Full, can `#include` StateTree headers | String-based, silent failures |
| API drift detection | Compile error (good) | Silent empty data (bad) |
| Code complexity | Normal UE code | Reflection boilerplate |
| Graceful degradation | Module simply doesn't load | Manual `IsAvailable()` checks |

## Verified: Build-Time Behavior

A concern was raised that UBT might fail to compile the optional module when the dependent plugin (StateTree) is not enabled for the current project. We tested this directly:

- **Project without StateTree**: UBT successfully compiled `FathomUELinkStateTree` because StateTree modules are pre-built as part of the engine installation, regardless of whether a specific project enables the plugin. `LoadingPhase: None` prevents runtime auto-loading, while UBT can still link against the engine's pre-compiled DLLs.
- **Project with StateTree**: Build succeeds, module dynamically loaded at runtime, auditor registers and works.

Key insight: `LoadingPhase: None` is a **runtime** control, not a **build-time** control. UBT compiles all modules in the `.uplugin` unconditionally. The engine's pre-built plugin DLLs are always available for linking.

## Important: Delegate Cleanup

The `OnModulesChanged` delegate captures `this`. Always store the handle and remove it in `ShutdownModule()` to avoid dangling pointers:

```cpp
// StartupModule
OnModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddLambda(...);

// ShutdownModule
if (OnModulesChangedHandle.IsValid())
{
    FModuleManager::Get().OnModulesChanged().Remove(OnModulesChangedHandle);
}
```

## Source

Pattern shared via Discord, based on production usage with Niagara as an optional dependency. This is a well-established UE community pattern.

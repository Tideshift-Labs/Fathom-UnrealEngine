# Initial Audit: FScopedSlowTask + Frame-Paced Ticker

How `UBlueprintAuditSubsystem` handles the post-load stale-check sweep without freezing the editor for minutes on schema bumps or first runs.

## The Problem

`BlueprintAuditSubsystem` runs a state machine on solution load:

1. **WaitingForRegistry**: poll `IAssetRegistry::IsLoadingAssets()`.
2. **BuildingList**: enumerate `GetAssetsByClass` for every audited type. Game thread.
3. **BackgroundHash**: dispatch MD5-comparison to `EAsyncExecution::ThreadPool`. Off the game thread.
4. **ProcessingStale**: per stale entry, `LoadObject<>` + `Gather*Data(...)` on the game thread, then dispatch markdown serialize + write to the threadpool.

Phase 4 is the expensive one. On a schema bump (`AuditSchemaVersion` is encoded into the audit output directory `vN/`, so any bump invalidates every existing audit file), the stale set is **every Blueprint, DataTable, DataAsset, Material, BehaviorTree, and UserDefinedStruct in the project**. On a realistic project (e.g. AfterpartyGame / RPG_InventorySystem) this is thousands of force-loads, each triggering transitive package loads and surfacing every linker warning under the sun (`Failed to load BlueprintGeneratedClass ... as Parent`, `bSelfContext == true, but no scope supplied`, etc.).

## Hard Constraint: Gather Is Game-Thread-Only

Every `FBlueprintAuditor::Gather*Data` method is documented as game-thread-only (`BlueprintAuditor.h:39, 42, 45, 61, 69, 77, 85, 93, 101`). This is not a "we couldn't make it work" constraint; it is a property of UE's reflection system, BP graph traversal, and `UEdGraph` / `UK2Node` containers. None of these are safe to walk from a worker thread.

**You cannot move Phase 4 to a background thread.** Phase 3 (file hashing) was successfully moved because it is pure file IO + MD5 over `.uasset` bytes, with zero `UObject` access. The same trick does not apply to Phase 4.

Backgrounding `LoadObject` itself is also off-limits: package loading wires into the BP compiler, asset thumbnail manager, KismetCompiler delegates, and dozens of other game-thread editor systems.

## Why Splash-Screen Loading Doesn't Help

The UE splash is shown during early engine init, before `FEngineLoop::Tick` and before the asset registry has finished its initial scan. `LoadObject<UBlueprint>` during splash either fails (registry not ready, BP compiler not initialised) or delays the editor's first window. There is no free CPU window in editor startup; whatever wall-clock the audit needs, the user pays.

## The Two-Path Solution

The fix is not to hide the cost (impossible) but to **make it survivable**. Two paths sized to the stale count:

### Small-batch path (`StaleEntries.Num() < 25`): one entry every `FramesPerStaleEntry` ticks

`LoadObject<UBlueprint>` is the **indivisible hitch unit**. A single heavy BP can take 100–500 ms on the game thread, and you cannot sub-divide that. Two consequences:

- Increasing batch-per-tick makes the freeze worse (`5 × 300 ms = 1.5 s` per tick).
- Decreasing batch-per-tick to `1` is the floor. Below that, the only knob is `1 every N frames`.

`1 every N frames` does **not** reduce the per-event freeze: the LoadObject still takes ~300ms. What it does is space the freezes out so the editor returns to interactive between hitches. With `N = 3`, after each ~300 ms hitch the editor gets ~33 ms of breathing room (at 60 fps) before the next one. This is purely a perceptual smoothing; total wall-clock goes up linearly with `N`.

For small stale counts (a handful of edits since last launch, normal day-to-day) this finishes in seconds and is visually invisible. The default `N = 3` is the result of this perception/wall-clock tradeoff; tune `FramesPerStaleEntry` in `BlueprintAuditSubsystem.h` if a different feel is wanted.

### Bulk path (`StaleEntries.Num() >= 25`): synchronous `FScopedSlowTask`

For schema bumps and first runs (hundreds to thousands of stale entries), invisible pacing produces minutes of mystery freezes interrupted by linker warning storms in the Output Log. `FScopedSlowTask` is the correct UE idiom: a modal progress dialog that pumps Slate during `EnterProgressFrame`, gives the user a `Cancel` button, and is exactly what UE itself uses for *Resave All Loaded*, *Validate Assets*, and *Fix Up Redirectors*, all of which also load every asset in a project.

```cpp
FScopedSlowTask SlowTask(Total, NSLOCTEXT("Fathom", ..., "Re-auditing assets..."));
SlowTask.MakeDialog(/*bShowCancelButton=*/ true);
for (int32 i = 0; i < Total; ++i)
{
    if (SlowTask.ShouldCancel()) break;
    SlowTask.EnterProgressFrame(1.0f, ...);
    ProcessSingleStaleEntry(StaleEntries[i]);
    // GC bookkeeping
}
```

The slow task is invoked synchronously from the ticker callback (`EStaleCheckPhase::ProcessingStaleWithProgress`). The ticker is on the game thread; the slow task body is on the game thread; Slate pumps inside `EnterProgressFrame` so the dialog redraws and the cancel button works. The threshold `25` is a heuristic chosen so that small day-to-day re-audits don't trigger a dialog but anything resembling a bulk migration does.

## GC Interaction (Be Careful Here)

A previous crash in `OnStaleCheckTick` during editor-startup GC was fixed in commit `7a31dfb`. Two interactions to keep correct:

1. **Manual GC every `GCInterval` (= 50) entries** runs on both paths, guarded by `!IsGarbageCollecting()`. The guard prevents collision with engine-driven GC.
2. **`FScopedSlowTask::EnterProgressFrame` pumps Slate**, which can let GC fire between iterations. Do not hold raw `UObject*` across the boundary inside `ProcessSingleStaleEntry`. The current code calls `LoadObject<>` then immediately `Gather*Data(...)` then dispatches a threadpool write capturing the resulting POD by `MoveTemp`; no `UObject*` survives past that scope. Audit any future change to this helper for the same property.

The threadpool `DispatchBackgroundWrite` lambdas capture POD `F*AuditData` by move, never `UObject*`. Safe.

## Cancellation Semantics

If the user clicks `Cancel` during the slow-task dialog:

- The loop breaks; remaining entries are skipped.
- `StaleProcessIndex` reflects how many were processed.
- `Done` still runs `SweepOrphanedAuditFiles` (read-only against the registry, harmless if interrupted).
- Cancelled entries are **automatically picked up on next editor launch** because their on-disk hashes still don't match. No bookkeeping needed.

## Why Not `1 every N frames` for the Bulk Path Too?

Wall-clock. With thousands of entries, `N = 3` triples a 4-minute migration to 12 minutes for purely perceptual benefit. The dialog reframes the same wait as "expected one-time work" rather than "mystery freeze" and gives a cancel button, which is the actual UX win.

## What This Doesn't Fix

- **Linker warning storm** still happens on a schema bump. Those `LogLinker: Warning` lines come from `LoadObject` walking exports of BPs whose parent BP packages are missing on disk (e.g. stale third-party content). They are correct diagnostic output and would happen during any bulk asset load. Not in scope.
- **Total wall-clock** of the migration. The bulk path makes the wait visible and cancelable; it does not make it shorter. Reducing wall-clock would require either (a) skipping some classes of asset, or (b) running the audit out-of-process via `BlueprintAuditCommandlet`. Both are bigger changes.

## Threshold Tuning

Both knobs live in `BlueprintAuditSubsystem.h`:

```cpp
static constexpr int32 FramesPerStaleEntry = 3;   // small-batch pacing
static constexpr int32 SlowTaskThreshold   = 25;  // small-batch <-> dialog cutoff
```

If users start hitting `1 < count < 25` cases that feel painful (e.g. 20 BPs after a big git pull producing 6 seconds of micro-stutter), drop `SlowTaskThreshold`. If the dialog feels intrusive on small-but-just-over-threshold cases, raise it.

## Files

- `Source/FathomUELink/Public/BlueprintAuditSubsystem.h`: phase enum, constants, helper decls.
- `Source/FathomUELink/Private/BlueprintAuditSubsystem.cpp`: `OnStaleCheckTick` branching, `ProcessSingleStaleEntry`, `RunStaleProcessingWithProgressDialog`.
- `Misc/ScopedSlowTask.h`: engine header, in `Core` (already a transitive dependency, no `Build.cs` change needed).

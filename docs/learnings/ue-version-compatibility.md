# UE Cross-Version Compatibility (5.5 / 5.6 / 5.7)

How to keep the plugin compiling cleanly across the UE versions we support, when we can only fully verify one of them locally.

## Constraint

Project minimum is UE 5.5. Local engine install is 5.7. We have no 5.5 or 5.6 source available on disk. Per CLAUDE.md, the policy has been: write code against 5.7, assume backward compat, fix concrete breakages as they're reported.

This works for stable APIs but bites on enum tail-additions, newly-added UPROPERTY fields, and renamed function signatures. The failure mode is a **hard compile error** in the missing-version build (the symbol literally doesn't exist in the older header), not a runtime fallback we can `if`-around.

## Concrete failure: enum case label for a 5.7-only enumerator

Adding `case EStateTreeExpressionOperand::Multiply: return TEXT("MUL");` to a switch in StateTree audit broke 5.6 builds. `Multiply` is a 5.7 addition; in 5.6 the enum stops at `Or`. Referencing the symbol in a case label is a hard compile error.

The fix is straightforward but easy to forget when developing only against 5.7.

## Pattern: `UE_VERSION_NEWER_THAN_OR_EQUAL`

Engine ships a comparison macro at `Runtime/Core/Public/Misc/EngineVersionComparison.h`. It is **not auto-included via `CoreMinimal.h`** (verified by grep), so every file that uses it needs the explicit include.

```cpp
#include "Misc/EngineVersionComparison.h"

switch (Operand)
{
case EFoo::Stable:   return TEXT("Stable");
case EFoo::AlsoOk:   return TEXT("AlsoOk");
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
case EFoo::NewIn57:  return TEXT("NewIn57");
#endif
default:             return TEXT("");
}
```

Sibling macros: `UE_VERSION_OLDER_THAN(major, minor, patch)`, `UE_VERSION_NEWER_THAN(major, minor, patch)`. Engine itself uses these heavily; grep `Engine/Plugins/Cameras/GameplayCameras` for live examples.

## Inverse: an enumerator that was *removed* in a newer version

If an enumerator exists in 5.5/5.6 but was deleted in 5.7, you can't reference its symbolic name on 5.7 (compile error there instead). The trick used in `BlueprintGraphAuditor.cpp` for `EMessageSeverity::CriticalError` is **ordinal comparison**:

```cpp
// CriticalError (removed in 5.7, value 0 in 5.5/5.6) is covered without
// referencing the deprecated enumerator.
if (Node->ErrorType <= EMessageSeverity::Error)
{
    Severity = TEXT("Error");
}
```

`EMessageSeverity` is ordered most-to-least severe, so `<= Error` includes the removed `CriticalError` (which has a lower numeric value) without naming it. Works when the enum has a meaningful ordinal ordering; doesn't work for arbitrary flag enums.

## Other surfaces beyond enums

The same problem applies to:

- **UPROPERTY fields added in newer versions.** Reading a field that doesn't exist on 5.5/5.6 is a compile error. Wrap field-access in version guards or feature-detect via reflection (`UStruct::FindPropertyByName` returning non-null).
- **Function signatures that changed.** A method whose argument list changed between versions needs an `#if`/`#else` block or an inline lambda that picks the right call.
- **Types that moved namespaces / were renamed.** Less common but happens; rare enough to fix on report.

`FInstancedPropertyBag`, `FStateTreeDelegateDispatcher`, and similar are stable in 5.5+ per their introduction dates and are safe to use unconditionally.

## How to spot risky symbols when developing only against 5.7

When writing a `switch` over an unfamiliar UE enum, **eyeball the tail of the enum declaration** in the 5.7 header for hints that the last entries are recent additions:

- Last entry in the enum list, especially if it has a verbose comment like the others were not getting.
- Comments explicitly mentioning "5.7" or "Added in".
- Recent file timestamps in the engine source (less reliable; engine ships rebuilt headers).
- Plugins that have lots of `UE_VERSION_NEWER_THAN_OR_EQUAL` in their own code typically signal that subsystem evolves often.

For StateTree, GAS, MovieScene, and other rapidly-evolving subsystems, default to suspicion. For Core, Engine, AIModule, and similar long-stable surfaces, less so.

## Test discipline

Without a local 5.5 / 5.6 install, the test path for these fixes is necessarily indirect:

- A friend or CI with the older engine builds the plugin and reports failures.
- Each reported failure becomes a one-symbol guard, confirmed via the engine version it was added in.
- The CHANGELOG entry calls out the affected versions explicitly so users can match what they're seeing.

Setting up a 5.5 or 5.6 install just for plugin verification would be cleaner but is out of scope today.

## Files

- `Source/FathomUELink/Private/Audit/BlueprintGraphAuditor.cpp`: ordinal-comparison pattern for `EMessageSeverity::CriticalError`.
- `Source/FathomUELinkStateTree/Private/StateTreeAuditor.cpp`: `UE_VERSION_NEWER_THAN_OR_EQUAL` guard for `EStateTreeExpressionOperand::Multiply`.
- `Misc/EngineVersionComparison.h`: where the comparison macros live; explicit include required.

# StateTree Audit: Decoding Dynamic Property Bags and Resolving Delegates

How the audit surfaces `FInstancedPropertyBag`, `FInstancedStruct`, and `OnDelegate` transition identities. Two patterns that reflection-only walkers cannot solve on their own.

## Problem

Plain `TFieldIterator<FProperty>` walking handles every static UE5 type. It does not handle:

1. **`FInstancedPropertyBag`**: a runtime-built `UScriptStruct` whose schema is determined at edit/cook time by the bag's owner. Walking the bag struct's static `FProperty` list yields a few internal handle members, never the user-visible properties.
2. **`FInstancedStruct`**: a wrapper holding a `UScriptStruct*` + raw memory. Static walking sees only the wrapper's internals.
3. **StateTree `OnDelegate` transitions**: the transition's `DelegateListener` field is an empty marker struct (`FStateTreeTransitionDelegateListener` in `StateTreeState.h:71` is literally `struct { GENERATED_BODY() }`). The dispatcher identity lives in the property binding system, not on the listener.

Symptoms before the fix (real user feedback on a UI menu StateTree):
- `Set Actor Property` task showed only the boolean execution flags (`bSetOnEnterState: True`...). The actual property being set (`bShowMouseCursor = true`) was invisible.
- `Call Widget Event` task showed only the `[Widget <- ...]` binding line. Widget Class, Event Name, and any Parameters were invisible.
- Four `OnDelegate` transitions all rendered as `OnDelegate -> X`, distinguishable only by their target state. The actual button delegate driving each was invisible.

An LLM reading the audit could not answer "which button click goes to which menu state" without opening the editor.

## Fix 1: Dynamic property bag and instanced struct decoding

In `AuditHelpers.cpp`, dispatch on the struct type at the top of `FormatStructValue` so every caller benefits (the FStructProperty case in `FormatPropertyValueImpl`, plus all the `TArray` / `TSet` / `TMap` element walkers that call `FormatStructValue` directly on inner struct elements):

```cpp
if (StructType == FInstancedPropertyBag::StaticStruct())
{
    const FInstancedPropertyBag* Bag = static_cast<const FInstancedPropertyBag*>(StructPtr);
    return FormatPropertyBagValue(*Bag, IndentDepth, Ctx);
}

if (StructType == FInstancedStruct::StaticStruct())
{
    const FInstancedStruct* Inst = static_cast<const FInstancedStruct*>(StructPtr);
    if (const UScriptStruct* InnerType = Inst->GetScriptStruct())
    {
        return FormatStructValue(InnerType, Inst->GetMemory(), IndentDepth, Ctx);
    }
    return TEXT("None");
}
```

Putting the dispatch inside `FormatStructValue` rather than `FormatPropertyValueImpl`'s FStructProperty branch matters: arrays of bags / instanced structs (`TArray<FInstancedStruct>`, etc.) also need unwrap, and those go through `FormatStructValue` directly per element without ever hitting the FStructProperty branch.

`FormatPropertyBagValue` walks the bag's dynamic schema directly via `TFieldIterator`. It does **not** delegate to `FormatStructValue`, which would be the obvious choice but crashes for property bags. Reason discovered the hard way:

- `UPropertyBag` overrides `InitializeStruct` (PropertyBag.cpp:4145) to increment a refcount on top of the default zero-fill behavior.
- `FormatStructValue` allocates a default-init buffer of `GetStructureSize()` bytes to compare each field against its default (line 627).
- For an empty / freshly-created bag, `GetStructureSize()` returns 0. `TArray<uint8>::SetNumZeroed(0)` produces an empty array; `GetData()` returns nullptr.
- `UScriptStruct::InitializeStruct` then trips `check(Dest)` at `Class.cpp:3785`. Editor crash on Blueprint audit when a BP contains a (possibly-empty) `FInstancedPropertyBag` field.

Fix: bag walker iterates fields against the bag's actual memory and skips the default-comparison buffer entirely. Bags by intent represent values the owner explicitly set, so emitting all populated fields is the desired behavior anyway. `FormatStructValue` also got a defensive `StructSize <= 0` early-return as a backstop for any other UScriptStruct subclass that returns 0 size.

The `FInstancedStruct` case still goes through `FormatStructValue`: it wraps a *real* concrete struct type with a normal positive size, so the default-comparison trick works as intended.

The fix lives in shared `AuditHelpers.cpp`, so DataAsset / BehaviorTree / Material / Blueprint / StateTree all benefit at once.

### Why this works for "Setup is missing"

Many StateTree BP tasks expose a `Setup` field that is internally an `FInstancedStruct` (e.g. `Set Actor Property` wraps its config in a struct keyed by the chosen Actor Class) or an `FInstancedPropertyBag` (parameter dictionaries). Either way, the static-FProperty walker bottoms out at the wrapper. Adding the two unwraps above makes the inner contents visible without any per-task customization.

## Fix 2: OnDelegate transition identity via property bindings

`FStateTreeTransitionDelegateListener` carries no data. The way the editor "knows" which dispatcher feeds a transition is via the same property binding system that wires task input pins. Each `OnDelegate` transition has an entry in `EditorData->GetPropertyEditorBindings()` whose:
- Target struct ID = the transition's own `FGuid ID`.
- Target property path contains `DelegateListener`.
- Source struct ID = the GUID of the task or evaluator that owns the dispatcher.
- Source property path = the dispatcher field name on that owner.

The auditor already had `GatherNodeBindings` (used for task input pins). Reusing it for transitions:

```cpp
const TArray<FStateTreePropertyBindingAuditData> TransitionBindings =
    GatherNodeBindings(EditorData, Transition.ID, BindableNames);
for (const auto& Binding : TransitionBindings)
{
    if (Binding.TargetProperty.Contains(TEXT("DelegateListener")) && !Binding.SourcePath.IsEmpty())
    {
        TransData.DelegateName = Binding.SourcePath;
        break;
    }
}
```

Output transformation (real example):
```
- OnDelegate -> ExitConfirm                              <- before
- OnDelegate (Exit (Button Clicked).OnTriggered) -> ExitConfirm   <- after
```

The fallback path (any non-empty source) covers UE version drift if the field name changes. Audited gracefully if the binding goes missing: renders `OnDelegate -> X` as before, no regression.

`OnEvent` is simpler: `Transition.RequiredEvent.Tag` and `RequiredEvent.PayloadStruct` are public on `FStateTreeEventDesc`. Read directly, render in parens.

## Cycle Detection: Why Depth-Only Is Fine

Asked whether struct cycles need a more reliable detector than `MaxInstancedRecursionDepth = 8`:

- **UObject cycles**: detected via `FFormatContext::Visited` (`TSet<const UObject*>`) at `AuditHelpers.cpp:262`. Triggered on `Instanced` UPROPERTY chains. Solid.
- **Struct cycles**: not possible at the C++ type level. `USTRUCT` cannot contain itself directly (infinite size). The only theoretical paths are `TInstancedStruct<Self>` (never seen in real UE assets) or wrapping in a UObject (caught by visited set).
- **PropertyBag cycles**: bags hold non-self types, structurally impossible.

So depth is not the cycle detector; the UObject visited set is. Depth is purely a backstop against pathological non-cyclic nesting. Current value of 8 covers realistic UE asset depth (parameters > nested struct > array of structs > inner struct chain rarely exceeds 5).

## UE Version Compatibility (5.5 / 5.6 / 5.7)

Verified against UE 5.7 source at `C:\Epic\Engines\UE_5.7\Engine\Plugins\Runtime\StateTree`. Assumed valid in 5.5+ per project minimum-version policy:

| Type / API | Verified in 5.7 | Available since |
|---|---|---|
| `FInstancedPropertyBag::StaticStruct()` | yes | 5.3 |
| `FInstancedStruct::StaticStruct()` | yes | 5.0 |
| `FStateTreeTransition::DelegateListener` | yes (`StateTreeState.h:137`) | 5.5 |
| `FStateTreeTransition::RequiredEvent` (with `PayloadStruct`) | yes | 5.5 |
| `FStateTreeEventDesc::Tag` / `PayloadStruct` | public, yes | 5.5 |
| Property binding system (`PropertyBindingBindingCollection`) | yes | 5.4 |

If a 5.5 / 5.6 user reports a missing field, the failure mode is "DelegateName / EventTag renders empty," not a compile break. Graceful degradation by design.

## Files

- `Source/FathomUELink/Private/Audit/AuditHelpers.cpp`: bag / instanced-struct dispatch in `FormatPropertyValueImpl`; new `FormatPropertyBagValue`.
- `Source/FathomUELinkStateTree/Private/StateTreeAuditTypes.h`: `DelegateName`, `EventTag`, `EventPayloadStruct` on `FStateTreeTransitionAuditData`.
- `Source/FathomUELinkStateTree/Private/StateTreeAuditor.cpp`: transition discriminator extraction in `GatherTransitionData`; rendering in `SerializeTransitions`.

## What This Doesn't Fix

- **Transition Conditions per binding**: if a transition's conditions themselves carry bindings, those follow the existing per-condition binding render (already handled).
- **Custom DetailCustomization paths**: any task that uses a Slate detail customizer to build its UI from non-reflected sources is still invisible. None observed in the friend's tree, but possible in heavy-customization plugins.
- **Schema bump**: not needed; output is additive within v15.

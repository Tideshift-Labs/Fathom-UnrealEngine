# Defensive Asset Traversal: Surviving Broken References in Audits

Auditors walk user content, and user content gets corrupted: assets deleted out from under references, partially-loaded packages, editor crashes mid-save. Every "this pointer is always valid" assumption in engine data eventually meets an asset where it is not. This doc catalogs the crash classes found (one from a real user crash report) and the guards now in place.

## Crash class 1: Null entries in graph link arrays

A real user hit an access violation in `BlueprintGraphAuditor.cpp`: `UEdGraphPin::LinkedTo` contained a null entry. Corrupted or partially-loaded graphs can hold:

- null entries in `Pin->LinkedTo`
- orphaned pins whose owning node is gone (`GetOwningNode()` is `check(OwningNode)` in `EdGraphPin.h`, so this is a fatal assert, not just an AV)
- null entries in `Graph->Nodes` and `Node->Pins`
- null graphs in `UBlueprint::UbergraphPages` / `FunctionGraphs` / `MacroGraphs`

**Guards**: skip null links/pins/nodes/graphs everywhere; use `GetOwningNodeUnchecked()` (returns null instead of asserting) and skip null owners. The same pattern applies to RigVM: `URigVMPin::GetLinkedTargetPins()` results and `URigVMNode::GetPins()` entries are null-checked in `ControlRigAuditor.cpp`.

## Crash class 2: Properties with broken type metadata

When the asset backing a property's type is deleted (a Blueprint class behind a `TSubclassOf`, a user-defined enum or struct), the `FProperty` survives with a nulled type pointer. The engine then crashes in three places that auditors call constantly:

| Property | Nulled field | Crash site |
|----------|-------------|------------|
| `FClassProperty` | `MetaClass` | `check(MetaClass)` in `GetCPPType` (`PropertyClass.cpp`) |
| `FSoftClassProperty` | `MetaClass` | `check(MetaClass)` in `GetCPPType` (`PropertySoftClassPtr.cpp`) |
| `FObjectProperty` | `PropertyClass` | `check(PropertyClass)` in `GetCPPType` |
| `FEnumProperty` | `Enum` | `check(Enum)` in `GetCPPType` |
| `FInterfaceProperty` | `InterfaceClass` | `checkSlow` + unguarded deref in `GetCPPType` |
| `FStructProperty` | `Struct` | unguarded deref in `GetCPPType`, `ExportTextItem`, and `Identical` |

`Identical` matters as much as the export calls: every CDO-diff loop calls `Prop->Identical(ValuePtr, DefaultPtr)` before formatting anything, so the crash happens before any value-formatting guard can help.

**Guards**: `FathomAuditHelpers::HasBrokenTypeMetadata(Prop)` recursively checks these fields (including container inner properties and struct fields; struct recursion terminates because a struct cannot contain itself at the C++ type level). Every property gather loop skips broken properties before calling `Identical` or formatting. `FathomAuditHelpers::GetSafeCPPType` returns `Unknown` instead of asserting; `FormatScalarProperty` returns `(unavailable)`.

One subtlety: `DataTableAuditor` rows emit a `(unavailable)` placeholder rather than skipping, because row values are positionally aligned with the numbered column legend.

## Crash class 3: Cycles in "tree" structures

Recursive walkers over parent/child object references (BehaviorTree composites, BlackboardData parent chains, StateTree state hierarchies, knot/reroute chains) assume acyclic data. Corruption can produce cycles, which turn the walk into infinite recursion and a stack-overflow crash.

**Guards**: thread a `TSet<const T*>& Visited` through each recursive walker using the `Visited.Add(Ptr, &bAlreadyVisited)` idiom (single hash lookup). The Blueprint knot trace and ControlRig reroute trace already had this; BehaviorTree, Blackboard, and StateTree walks now do too.

## Crash class 4: Property chains resolved against the wrong container

Found via a real editor crash on startup auditing PCG graphs (Electric Dreams sample). Some engine systems cache `FProperty` chains that do **not** belong to the object you hold. PCG's `FPCGSettingsOverridableParam::Properties` is the example: for subgraph nodes the chain points into the referenced subgraph's user-parameter property bag, and for Blueprint elements into the Blueprint class, not into the `UPCGSettings` object. Walking such a chain with `ContainerPtrToValuePtr` against the wrong container reads garbage memory; the resulting fake `UObject*` then crashes inside `ExportText` (`GetExportPath` dereferencing a garbage class pointer). Nothing is null and no type metadata is broken, so crash classes 1 and 2 guards do not catch it.

**Guards**: before walking a cached property chain, verify ownership: `Container->GetClass()->IsChildOf(Chain[0]->GetOwnerStruct())`. Skip (or resolve against the correct owner) when it fails. See `GatherSettingsValues` in `PCGGraphAuditor.cpp`.

A related case in the same family: engine accessors that recurse through reference chains without a cycle guard (`UPCGGraphInstance::GetGraph()` walks instance-of-instance chains). Editing code prevents cycles, but corrupted assets can still contain them, and the stack overflow then happens inside engine code where our visited-set guards cannot reach. Replicate the walk locally with a visited set instead of calling the engine accessor (`ResolveConcreteGraph` in `PCGGraphAuditor.cpp`).

## Rules of thumb for new auditors

1. Never range-for over an engine pointer array (`Nodes`, `Pins`, `LinkedTo`, `Children`) without a null skip on the element.
2. Prefer `*Unchecked()` accessor variants where they exist; the checked ones are fatal asserts.
3. Call `HasBrokenTypeMetadata` before `Identical` / `ExportTextItem` / `GetCPPType` on any property that came from user content.
4. Any recursive walk over object references needs a visited set, even if the structure is "always" a tree. This includes engine accessors that recurse internally; replicate them locally if they have no guard.
5. Skipping quietly is fine for diff-style sections; emit a placeholder where output is positional (numbered columns, table rows) or referenced elsewhere (parameter names used by override pins).
6. A cached `FProperty` chain is only as trustworthy as its owner. Verify `GetOwnerStruct()` against the container's class before any `ContainerPtrToValuePtr` walk; valid-looking chains can belong to a different object entirely.

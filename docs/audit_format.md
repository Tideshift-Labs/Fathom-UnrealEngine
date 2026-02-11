# Blueprint Audit Markdown Format

The audit output is Markdown, optimized for LLM consumption (more token-efficient than JSON). Sections with no data are omitted entirely.

## Full Example

````markdown
# WBP_MainMenu
Path: /Game/UI/WBP_MainMenu.WBP_MainMenu
Parent: /Script/CommonUI.CommonActivatableWidget
Type: Normal
Hash: fe020519d8ca4cf5b2e8690bd0bfabca

## Variables
| Name | Type | Category | Editable | Replicated |
|------|------|----------|----------|------------|
| PlayerName | String | Default | Yes | No |

## Property Overrides
- bAutoActivate = True

## Interfaces
- IMenuInterface

## Components
| Name | Class |
|------|-------|
| RootComponent | SceneComponent |

## Timelines
| Name | Length | Loop | AutoPlay | Float | Vector | Color | Event |
|------|--------|------|----------|-------|--------|-------|-------|
| FadeTimeline | 2.00 | No | No | 1 | 0 | 0 | 0 |

## Widget Tree
- CanvasPanel_0 (CanvasPanel)
  - Button_Start (Button) [var]
  - Text_Title (TextBlock)
  - WBP_TemplateLayout (WBP_TemplateLayout_C)
    - VerticalBox_Content (VerticalBox) [slot:ContentSlot]

## EventGraph
| Id | Type | Name | Details |
|----|------|------|---------|
| 0 | Event | Event BeginPlay | |
| 1 | CallFunction | IsValid | KismetSystemLibrary, pure |
| 2 | Branch | Branch | |
| 3 | CallFunction | PlayAnimation | UserWidget, not-native |
| 4 | VariableGet | PlayerName | pure |
| 5 | VariableSet | bIsActive | |

Exec: 0->2, 2-[True]->3, 2-[False]->5
Data: 4.PlayerName->1.Object, 1.ReturnValue->2.Condition

## Function: GetFormattedName(Prefix: String) -> ReturnValue: String
| Id | Type | Name | Details |
|----|------|------|---------|
| 0 | FunctionEntry | GetFormattedName | |
| 1 | CallFunction | Concat_StrStr | KismetStringLibrary, pure |
| 2 | VariableGet | PlayerName | pure |
| 3 | FunctionResult | Return | |

Exec: 0->3
Data: 0.Prefix->1.A, 2.PlayerName->1.B, 1.ReturnValue->3.ReturnValue
````

## Format Details

**Header lines**: Name (H1 heading), Path, Parent, Type, Hash. Used for staleness detection and quick identification.

**Node tables** use `| Id | Type | Name | Details |` columns. The Details column contains target class, flags (pure, latent, not-native), and hardcoded default input values.

**Edge one-liners**: Compact notation after each node table.
- Exec edges: `SrcId-[PinName]->DstId`. Pin name omitted when it is "then": `0->1`.
- Data edges: `SrcId.PinName->DstId.PinName`.

**Graph headings**:
- Event graphs: `## EventGraph` (or the graph name)
- Functions: `## Function: Name(params) -> returns`
- Macros: `## Macro: Name`

**Widget tree**: Indented list with `[var]` suffix for variable widgets and `[slot:Name]` suffix for content placed in named slots of template widgets.

**Node types**: `FunctionEntry`, `FunctionResult`, `Event`, `CustomEvent`, `CallFunction`, `Branch`, `Sequence`, `VariableGet`, `VariableSet`, `MacroInstance`, `Timeline`, `Other`.

Reroute/knot nodes are skipped; edges trace through them to the real endpoints.

#pragma once

// Compatibility header: detect the correct ControlRig Blueprint header.
// - UE 5.6:  ControlRigBlueprint.h
// - UE 5.5:  ControlRigBlueprintLegacy.h
// - UE 5.7+: Header removed; ControlRig-specific auditing is disabled at compile time.
//            ControlRig assets will still be audited, just via the generic Blueprint path.

#if __has_include("ControlRigBlueprint.h")
	#include "ControlRigBlueprint.h"
	#define FATHOM_HAS_CONTROLRIG_BLUEPRINT 1
#elif __has_include("ControlRigBlueprintLegacy.h")
	#include "ControlRigBlueprintLegacy.h"
	#define FATHOM_HAS_CONTROLRIG_BLUEPRINT 1
#else
	#define FATHOM_HAS_CONTROLRIG_BLUEPRINT 0
#endif

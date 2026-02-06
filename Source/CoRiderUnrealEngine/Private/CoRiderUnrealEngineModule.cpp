#include "CoRiderUnrealEngineModule.h"

#define LOCTEXT_NAMESPACE "FCoRiderUnrealEngineModule"

void FCoRiderUnrealEngineModule::StartupModule()
{
	// Module startup — subsystem auto-registers via UCLASS
}

void FCoRiderUnrealEngineModule::ShutdownModule()
{
	// Module shutdown
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FCoRiderUnrealEngineModule, CoRiderUnrealEngine)

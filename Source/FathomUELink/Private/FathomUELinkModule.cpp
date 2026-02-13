#include "FathomUELinkModule.h"
#include "BlueprintAuditor.h"

#define LOCTEXT_NAMESPACE "FFathomUELinkModule"

void FFathomUELinkModule::StartupModule()
{
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: FathomUELink module loaded."));
}

void FFathomUELinkModule::ShutdownModule()
{
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: FathomUELink module unloaded."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFathomUELinkModule, FathomUELink)

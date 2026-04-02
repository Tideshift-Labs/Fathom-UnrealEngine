#include "FathomUELinkModule.h"

DEFINE_LOG_CATEGORY(LogFathomUELink);

#define LOCTEXT_NAMESPACE "FFathomUELinkModule"

void FFathomUELinkModule::StartupModule()
{
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: FathomUELink module loaded."));

	LoadOptionalModules();

	OnModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddLambda(
		[this](FName ModuleName, EModuleChangeReason ChangeReason)
		{
			if (ChangeReason == EModuleChangeReason::ModuleLoaded)
			{
				LoadOptionalModules();
			}
		});
}

void FFathomUELinkModule::ShutdownModule()
{
	if (OnModulesChangedHandle.IsValid())
	{
		FModuleManager::Get().OnModulesChanged().Remove(OnModulesChangedHandle);
		OnModulesChangedHandle.Reset();
	}

	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: FathomUELink module unloaded."));
}

void FFathomUELinkModule::LoadOptionalModules()
{
	const FName StateTreeRequiredPlugins[] = { TEXT("StateTreeModule") };
	LoadOptionalModuleIfPluginsPresent(
		TEXT("FathomUELinkStateTree"),
		StateTreeRequiredPlugins);
}

void FFathomUELinkModule::LoadOptionalModuleIfPluginsPresent(
	const FName& ModuleToLoad,
	TArrayView<const FName> RequiredPlugins)
{
	if (FModuleManager::Get().IsModuleLoaded(ModuleToLoad))
	{
		return;
	}

	for (const FName& PluginName : RequiredPlugins)
	{
		if (!FModuleManager::Get().IsModuleLoaded(PluginName))
		{
			return;
		}
	}

	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: Loading optional module '%s'"), *ModuleToLoad.ToString());
	FModuleManager::Get().LoadModule(ModuleToLoad);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFathomUELinkModule, FathomUELink)

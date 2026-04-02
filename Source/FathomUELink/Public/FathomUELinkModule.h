#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

FATHOMUELINK_API DECLARE_LOG_CATEGORY_EXTERN(LogFathomUELink, Log, All);

class FFathomUELinkModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void LoadOptionalModules();
	void LoadOptionalModuleIfPluginsPresent(const FName& ModuleToLoad, TArrayView<const FName> RequiredPlugins);

	FDelegateHandle OnModulesChangedHandle;
};

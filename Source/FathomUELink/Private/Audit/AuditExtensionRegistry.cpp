#include "Audit/AuditExtensionRegistry.h"
#include "FathomUELinkModule.h"

FAuditExtensionRegistry& FAuditExtensionRegistry::Get()
{
	static FAuditExtensionRegistry Instance;
	return Instance;
}

void FAuditExtensionRegistry::RegisterExtension(FExtension&& Extension)
{
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: Registered audit extension '%s'"), *Extension.Name.ToString());
	Extensions.Add(MoveTemp(Extension));
}

void FAuditExtensionRegistry::UnregisterExtension(FName Name)
{
	Extensions.RemoveAll([Name](const FExtension& Ext) { return Ext.Name == Name; });
	UE_LOG(LogFathomUELink, Log, TEXT("Fathom: Unregistered audit extension '%s'"), *Name.ToString());
}

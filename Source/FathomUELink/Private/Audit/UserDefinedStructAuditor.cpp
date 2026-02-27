#include "Audit/UserDefinedStructAuditor.h"

#include "Audit/AuditHelpers.h"
#include "Audit/AuditFileUtils.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/UnrealType.h"

/**
 * Recursively checks whether a property (or its inner properties in containers)
 * is an FSoftClassProperty whose MetaClass is null. UE asserts when
 * ExportTextItem_Direct is called on such a property, so we skip it.
 */
static bool HasInvalidSoftClassMeta(const FProperty* Prop)
{
	if (const FSoftClassProperty* SCP = CastField<FSoftClassProperty>(Prop))
	{
		return SCP->MetaClass == nullptr;
	}
	if (const FArrayProperty* AP = CastField<FArrayProperty>(Prop))
	{
		return HasInvalidSoftClassMeta(AP->Inner);
	}
	if (const FSetProperty* SP = CastField<FSetProperty>(Prop))
	{
		return HasInvalidSoftClassMeta(SP->ElementProp);
	}
	if (const FMapProperty* MP = CastField<FMapProperty>(Prop))
	{
		return HasInvalidSoftClassMeta(MP->KeyProp) || HasInvalidSoftClassMeta(MP->ValueProp);
	}
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			if (HasInvalidSoftClassMeta(*It)) return true;
		}
	}
	return false;
}

FUserDefinedStructAuditData FUserDefinedStructAuditor::GatherData(const UUserDefinedStruct* Struct)
{
	FUserDefinedStructAuditData Data;

	Data.Name = Struct->GetName();
	Data.Path = Struct->GetPathName();
	Data.PackageName = Struct->GetOutermost()->GetName();
	Data.SourceFilePath = FAuditFileUtils::GetSourceFilePath(Data.PackageName);
	Data.OutputPath = FAuditFileUtils::GetAuditOutputPath(Data.PackageName);

	// Allocate a temp buffer to read default values from.
	// Only attempt this when the struct is fully compiled; structs with
	// errors or pending recompilation may have no valid default instance,
	// which causes a crash inside InitializeDefaultValue.
	const int32 StructSize = Struct->GetStructureSize();
	uint8* DefaultBuffer = nullptr;
	if (StructSize > 0 && Struct->Status == EUserDefinedStructureStatus::UDSS_UpToDate)
	{
		DefaultBuffer = static_cast<uint8*>(FMemory::Malloc(StructSize, Struct->GetMinAlignment()));
		Struct->InitializeDefaultValue(DefaultBuffer);
	}

	for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
	{
		const FProperty* Prop = *PropIt;

		FStructFieldDef Field;

		// UDS properties have authored display names; fall back to internal name
		Field.Name = Struct->GetAuthoredNameForField(Prop);
		if (Field.Name.IsEmpty())
		{
			Field.Name = Prop->GetName();
		}

		FString ExtendedType;
		Field.Type = Prop->GetCPPType(&ExtendedType);
		Field.Type += ExtendedType;

		// Export default value from the initialized buffer.
		// Skip properties with invalid FSoftClassProperty metadata to avoid
		// an assert inside ExportTextItem_Direct (UE5 PropertySoftClassPtr.cpp:57).
		if (DefaultBuffer && !HasInvalidSoftClassMeta(Prop))
		{
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(DefaultBuffer);
			Prop->ExportTextItem_Direct(Field.DefaultValue, ValuePtr, nullptr, nullptr, PPF_None);
		}

		Data.Fields.Add(MoveTemp(Field));
	}

	// Clean up the temp buffer
	if (DefaultBuffer)
	{
		Struct->DestroyStruct(DefaultBuffer);
		FMemory::Free(DefaultBuffer);
	}

	return Data;
}

FString FUserDefinedStructAuditor::SerializeToMarkdown(const FUserDefinedStructAuditData& Data)
{
	FString Result;
	Result.Reserve(1024);

	// Header
	Result += FString::Printf(TEXT("# %s\n"), *Data.Name);
	Result += FString::Printf(TEXT("Path: %s\n"), *Data.Path);

	if (!Data.SourceFilePath.IsEmpty())
	{
		Result += FString::Printf(TEXT("Hash: %s\n"), *FAuditFileUtils::ComputeFileHash(Data.SourceFilePath));
	}

	// Fields
	if (Data.Fields.Num() > 0)
	{
		Result += FString::Printf(TEXT("\n## Fields (%d)\n"), Data.Fields.Num());
		for (int32 i = 0; i < Data.Fields.Num(); ++i)
		{
			const FStructFieldDef& Field = Data.Fields[i];
			const FString CleanedDefault = FathomAuditHelpers::CleanExportedValue(Field.DefaultValue);
			if (CleanedDefault.IsEmpty())
			{
				Result += FString::Printf(TEXT("%d. %s (%s)\n"), i + 1, *Field.Name, *Field.Type);
			}
			else
			{
				Result += FString::Printf(TEXT("%d. %s (%s) = %s\n"), i + 1, *Field.Name, *Field.Type, *CleanedDefault);
			}
		}
	}

	return Result;
}

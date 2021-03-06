/*
 * PROJECT:     ReactOS WdfLdr driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WdfLdr driver - common functions
 * COPYRIGHT:   Copyright 2019 mrmks04 (mrmks04@yandex.ru)
 */


#include "common.h"
#include "ntddk_ex.h"

#include <ntintsafe.h>
#include <ntstrsafe.h>


NTSTATUS
NTAPI
AuxKlibQueryModuleInformation(
    _In_ PULONG InformationLenght,
    _In_ ULONG SizePerModule,
    _Inout_ PRTL_MODULE_EXTENDED_INFO ModuleInfo
);


#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AuxKlibQueryModuleInformation)
#endif


VOID
NTAPI
GetNameFromPath(
    _In_ PUNICODE_STRING Path,
    _Out_  PUNICODE_STRING Name)
{
    PWCHAR pNextSym;
    PWCHAR pCurrSym;

    if (Path->Length < sizeof(WCHAR))
    {
        Name->Length = 0;
        Name->Buffer = NULL;
        return;
    }
    
    Name->Buffer = Path->Buffer + (Path->Length / 2) - 1;
    Name->Length = sizeof(WCHAR);

    for (pNextSym = Name->Buffer; ; Name->Buffer = pNextSym)
    {
        if (pNextSym < Path->Buffer)
        {
            Name->Length -= sizeof(WCHAR);
            ++Name->Buffer;
            goto end;
        }
        pCurrSym = Name->Buffer;

        if (*pCurrSym == '\\')
        {
            break;
        }
        pNextSym = pCurrSym - 1;
        Name->Length += sizeof(WCHAR);
    }

    ++Name->Buffer;
    Name->Length -= sizeof(WCHAR);

    if (Name->Length == sizeof(WCHAR))
    {
        Name->Buffer = NULL;
        Name->Length = 0;
    }

end:
    Name->MaximumLength = Name->Length;
}


NTSTATUS
NTAPI
GetImageName(
    _In_ PUNICODE_STRING DriverServiceName,
    _In_ ULONG Tag,
    _In_ PUNICODE_STRING ImageName)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING name;
    UNICODE_STRING path;
    PKEY_VALUE_PARTIAL_INFORMATION pKeyValPartial = NULL;
    HANDLE KeyHandle = NULL;
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"ImagePath");

    ImageName->Length = 0;
    ImageName->Buffer = NULL;
    name.Length = 0;
    name.Buffer = NULL;

    InitializeObjectAttributes(&objectAttributes, DriverServiceName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwOpenKey(&KeyHandle, KEY_READ, &objectAttributes);

    if (!NT_SUCCESS(status)) 
    {
        goto error;
    }

    status = FxLdrQueryData(KeyHandle, &ValueName, WDFLDR_TAG, &pKeyValPartial);
    if (!NT_SUCCESS(status)) 
    {
        goto error;
    }

    if (pKeyValPartial->Type != REG_SZ &&
        pKeyValPartial->Type != REG_EXPAND_SZ) 
    {
        status = STATUS_OBJECT_TYPE_MISMATCH;
        goto error;
    }

    if (pKeyValPartial->DataLength == 0 ||
        pKeyValPartial->DataLength > 0xFFFF) 
    {
        status = STATUS_INVALID_PARAMETER;    
        goto error;
    }

    path.Buffer = (PWCH)pKeyValPartial->Data;
    path.Length = (USHORT)pKeyValPartial->DataLength;
    path.MaximumLength = (USHORT)pKeyValPartial->DataLength;

    if (pKeyValPartial->DataLength >= sizeof(WCHAR) &&
        !*(((WCHAR*)&pKeyValPartial->Data) + pKeyValPartial->DataLength / sizeof(WCHAR)))
    {
        path.Length = (USHORT)pKeyValPartial->DataLength - sizeof(WCHAR);
    }

    GetNameFromPath(&path, &name);

    if (name.Length == 0) 
    {
        status = STATUS_INVALID_PARAMETER;
        __DBGPRINT(("ERROR: GetNameFromPathW could not find a name, status 0x%x\n", status));
        goto error;
    }

    status = RtlUShortAdd(name.Length, 2u, &ImageName->Length);
        
    if (!NT_SUCCESS(status)) 
    {
        status = STATUS_INTEGER_OVERFLOW;
        __DBGPRINT(("ERROR: size computation failed with Status 0x%x\n", status));
        goto error;
    }
    
    ImageName->Buffer = ExAllocatePoolWithTag(PagedPool, ImageName->Length, Tag);

    if (ImageName->Buffer != NULL) 
    {
        RtlZeroMemory(ImageName->Buffer, ImageName->Length);
        ImageName->MaximumLength = ImageName->Length;
        ImageName->Length = 0;
        RtlCopyUnicodeString(ImageName, &name);

        __DBGPRINT(("Version Image Name \"%wZ\"\n", ImageName));
        goto end;
    }
    else 
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        __DBGPRINT(("ERROR: ExAllocatePoolWithTag failed with Status 0x%x\n", status));
    }

error:
    __DBGPRINT(("ERROR: GetImageName failed with status 0x%x\n", status));

end:
    if (KeyHandle) 
    {
        ZwClose(KeyHandle);
    }
    if (pKeyValPartial) 
    {
        ExFreePoolWithTag(pKeyValPartial, 0);
    }

    return status;
}


PCHAR
NTAPI
GetFileName(
    _In_ PCHAR Path)
{
    size_t length;
    PCHAR currentSym;

    if (Path == NULL)
        return NULL;

    length = strlen(Path);
    if (length == 0)
        return NULL;

    for (currentSym = &Path[length]; currentSym >= Path; --currentSym) 
    {
        if (*currentSym == '\\')
            return currentSym + 1;
    }

    return Path;
}

// TODO: move to aux_klib.lib
NTSTATUS
NTAPI
AuxKlibQueryModuleInformation(
    _In_ PULONG InformationLenght,
    _In_ ULONG SizePerModule,
    _Inout_ PRTL_MODULE_EXTENDED_INFO ModuleInfo)
{
    NTSTATUS status;
    PRTL_PROCESS_MODULES pSysInfo;
    ULONG sysInfoLen;
    ULONG modulesSize;
    ULONG ResultLength;
    PULONG pInfoLength;
    ULONG index;
    RTL_PROCESS_MODULES systemInformation;

    PAGED_CODE();

    pInfoLength = InformationLenght;
    if (gKlibInitialized != 1) 
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (pfnRtlQueryModuleInformation != NULL)
    {
        return pfnRtlQueryModuleInformation(InformationLenght, SizePerModule, ModuleInfo);
    }

    status = STATUS_SUCCESS;
    if (SizePerModule != sizeof(RTL_MODULE_BASIC_INFO) && SizePerModule != sizeof(RTL_MODULE_EXTENDED_INFO))
    {
        return STATUS_INVALID_PARAMETER_2;
    }

    if ((SIZE_T)ModuleInfo & 3)
    {
        return STATUS_INVALID_PARAMETER_3;
    }

    pSysInfo = &systemInformation;
    for (sysInfoLen = sizeof(RTL_PROCESS_MODULES); ; sysInfoLen = ResultLength)
    {
        status = ZwQuerySystemInformation(SystemModuleInformation, pSysInfo, sysInfoLen, &ResultLength);
        if (NT_SUCCESS(status))
            break;

        if (status != STATUS_INFO_LENGTH_MISMATCH)
            goto clean;

        if (pSysInfo != &systemInformation)
            ExFreePoolWithTag(pSysInfo, WDFLDR_TAG);

        pSysInfo = ExAllocatePoolWithQuotaTag(PagedPool, ResultLength, WDFLDR_TAG);
        
        if (pSysInfo == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (SizePerModule * pSysInfo->NumberOfModules > 0xFFFFFFFF) 
    {
        status = STATUS_INTEGER_OVERFLOW;
        goto clean;
    }

    modulesSize = SizePerModule * pSysInfo->NumberOfModules;

    if (ModuleInfo == NULL)
    {
        goto clean;
    }

    if (*pInfoLength < modulesSize) 
    {
        status = STATUS_BUFFER_TOO_SMALL;
        goto end;
    }

    if (pSysInfo->NumberOfModules == 0)
        goto end;

    for (index = 0; index < pSysInfo->NumberOfModules; ++index, ModuleInfo++) 
    {
        ModuleInfo->BasicInfo.ImageBase = pSysInfo->Modules[index].ImageBase;
        ModuleInfo->ImageSize = pSysInfo->Modules[index].ImageSize;
        ModuleInfo->FileNameOffset = pSysInfo->Modules[index].OffsetToFileName;
        RtlCopyMemory(ModuleInfo->FullPathName, pSysInfo->Modules[index].FullPathName, sizeof(ModuleInfo->FullPathName));
    }

end:
    *pInfoLength = modulesSize;
clean:
    if (pSysInfo != &systemInformation)
        ExFreePoolWithTag(pSysInfo, WDFLDR_TAG);

    return status;
}


NTSTATUS
NTAPI
GetImageBase(
    _In_ PCUNICODE_STRING ImageName,
    _Out_  PVOID* ImageBase,
    _Out_  PULONG ImageSize)
{
    PCHAR fileName;
    PRTL_MODULE_EXTENDED_INFO pModuleInfoBuffer;
    STRING ansiImageName;
    size_t fileNameLength;
    ULONG informationLength;
    ULONG totalSize;
    ULONG numberOfBytes;
    NTSTATUS status;
    ULONG i;

    pModuleInfoBuffer = NULL;
    *ImageBase = 0;
    *ImageSize = 0;
    ansiImageName.Length = 0;
    ansiImageName.MaximumLength = 0;
    ansiImageName.Buffer = NULL;
    informationLength = 0;
    status = RtlUnicodeStringToAnsiString(&ansiImageName, ImageName, TRUE);

    if (!NT_SUCCESS(status) || ansiImageName.Buffer == NULL)
    {
        __DBGPRINT(("ERROR: RtlUnicodeStringToAnsiString failed with Status 0x%x\n", status));

        ansiImageName.Buffer = NULL;
        goto end;
    }

    ansiImageName.Buffer[ansiImageName.Length] = 0;
    fileName = GetFileName(ansiImageName.Buffer);
    fileNameLength = strlen(fileName);

    if (fileName == NULL || fileNameLength == 0)
    {
        status = STATUS_OBJECT_NAME_NOT_FOUND;
        goto clean;
    }
    totalSize = 0;

    for (;;)
    {
        numberOfBytes = 0;
        status = AuxKlibQueryModuleInformation(&numberOfBytes, sizeof(RTL_MODULE_EXTENDED_INFO), NULL);

        if (!NT_SUCCESS(status) || !numberOfBytes)
        {
            if (!WdfLdrDiags)
                goto clean;
            break;
        }
        status = RtlULongAdd(numberOfBytes, totalSize, &informationLength);

        if (!NT_SUCCESS(status))
        {
            __DBGPRINT(("ERROR: RtlUlongAdd failed with Status 0x%x\n", status));
        }
        else
        {
            numberOfBytes = informationLength;
        }

        pModuleInfoBuffer = ExAllocatePoolWithTag(PagedPool, numberOfBytes, WDFLDR_TAG);

        if (pModuleInfoBuffer == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            __DBGPRINT(("ERROR: ExAllocatePoolWithTag failed with Status 0x%x\n", status));

            goto end;
        }

        RtlZeroMemory(pModuleInfoBuffer, numberOfBytes);
        informationLength = numberOfBytes;
        status = AuxKlibQueryModuleInformation(&informationLength, sizeof(RTL_MODULE_EXTENDED_INFO), pModuleInfoBuffer);

        if (status != STATUS_BUFFER_TOO_SMALL)
            break;

        ExFreePoolWithTag(pModuleInfoBuffer, WDFLDR_TAG);
        totalSize += sizeof(RTL_MODULE_EXTENDED_INFO);
        pModuleInfoBuffer = NULL;

        if (totalSize >= sizeof(RTL_MODULE_EXTENDED_INFO) * 10)
            goto clean;
    }

    if (!NT_SUCCESS(status))
    {
        __DBGPRINT(("ERROR: AuxKlibQueryModuleInformation failed with Status 0x%x\n", status));

        goto end;
    }

    for ( i = 0; i < informationLength / sizeof(RTL_MODULE_EXTENDED_INFO); i++)
    {
        if (pModuleInfoBuffer[i].FileNameOffset < 0x100 &&
            strncmp(pModuleInfoBuffer[i].FullPathName, fileName, fileNameLength) == 0)
        {
            *ImageBase = pModuleInfoBuffer[i].BasicInfo.ImageBase;
            *ImageSize = pModuleInfoBuffer[i].ImageSize;
            break;
        }
    }

end:
    if (pModuleInfoBuffer != NULL)
        ExFreePoolWithTag(pModuleInfoBuffer, WDFLDR_TAG);
clean:
    if (ansiImageName.Buffer)
        RtlFreeAnsiString(&ansiImageName);

    return status;
}


BOOLEAN
NTAPI
ServiceCheckBootStart(
    _In_ PUNICODE_STRING Service)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objectAttributes;
    HANDLE keyHandle = NULL;
    BOOLEAN result = FALSE;
    ULONG value;
    UNICODE_STRING valueName = RTL_CONSTANT_STRING(L"Start");

    InitializeObjectAttributes(&objectAttributes, Service, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenKey(&keyHandle, KEY_READ, &objectAttributes);    

    if (status != STATUS_OBJECT_NAME_NOT_FOUND) 
    {
        if (NT_SUCCESS(status)) 
        {
            status = FxLdrQueryUlong(keyHandle, &valueName, &value);
            if (NT_SUCCESS(status))
            {
                result = value == 0;
            }
        }
        else
        {
            __DBGPRINT(("ZwOpenKey(%wZ) failed: %08X\n", Service, status));
        }
    }

    if (keyHandle)
        ZwClose(keyHandle);

    return result;
}


NTSTATUS
NTAPI
FxLdrQueryUlong(
    _In_ HANDLE KeyHandle,
    _In_ PUNICODE_STRING ValueName,
    _Out_  PULONG Value)
{
    NTSTATUS status;
    ULONG resultLength;
    KEY_VALUE_PARTIAL_INFORMATION keyValue;

    keyValue.DataLength = 0;
    keyValue.TitleIndex = 0;
    keyValue.Type = 0;
    keyValue.Data[0] = 0;
    status = ZwQueryValueKey(KeyHandle, ValueName, KeyValuePartialInformation, &keyValue, sizeof(keyValue), &resultLength);

    if (NT_SUCCESS(status)) 
    {
        if (keyValue.Type != REG_DWORD || keyValue.DataLength != 4) 
        {
            status = STATUS_INVALID_BUFFER_SIZE;
        }
        else 
        {
            *Value = keyValue.Data[0];
            status = STATUS_SUCCESS;
        }
    }
    else 
    {
        __DBGPRINT(("ERROR: ZwQueryValueKey failed with Status 0x%x\n", status));
    }

    return status;
}


NTSTATUS
NTAPI
FxLdrQueryData(
    _In_ HANDLE KeyHandle,
    _In_ PUNICODE_STRING ValueName,
    _In_ ULONG Tag,
    _Out_  PKEY_VALUE_PARTIAL_INFORMATION* KeyValPartialInfo)
{
    PKEY_VALUE_PARTIAL_INFORMATION pKeyInfo;
    NTSTATUS status;
    ULONG resultLength;

    *KeyValPartialInfo = NULL;
    for (;;)
    {
        status = ZwQueryValueKey(KeyHandle, ValueName, KeyValuePartialInformation, NULL, 0, &resultLength);
        if (status != STATUS_BUFFER_TOO_SMALL) 
        {
            if (!NT_SUCCESS(status)) 
            {
                __DBGPRINT(("ERROR: ZwQueryValueKey failed with status 0x%x\n", status));
            }

            return status;
        }

        status = RtlULongAdd(resultLength, 0xCu, &resultLength);
        if (!NT_SUCCESS(status)) 
        {
            __DBGPRINT(("ERROR: Computing length of data under %wZ failed with status 0x%x\n", ValueName, status));

            return status;
        }

        pKeyInfo = ExAllocatePoolWithTag(PagedPool, resultLength, Tag);

        if (pKeyInfo == NULL)
        {
            break;
        }

        RtlZeroMemory(pKeyInfo, resultLength);
        status = ZwQueryValueKey(
            KeyHandle,
            ValueName,
            KeyValuePartialInformation,
            pKeyInfo,
            resultLength,
            &resultLength);

        if (NT_SUCCESS(status)) 
        {
            *KeyValPartialInfo = pKeyInfo;
            return status;
        }

        ExFreePoolWithTag(pKeyInfo, WDFLDR_TAG);

        if (status != STATUS_BUFFER_TOO_SMALL) 
        {
            __DBGPRINT(("ERROR: ZwQueryValueKey (%wZ) failed with Status 0x%x\n", ValueName, status));

            return status;
        }
    }

    __DBGPRINT(("ERROR: ExAllocatePoolWithTag failed with Status 0x%x\n", STATUS_INSUFFICIENT_RESOURCES));

    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID
FreeString(
    _In_ PUNICODE_STRING String)
{
    if (String != NULL && String->Buffer != NULL)
    {
        ExFreePoolWithTag(String->Buffer, WDFLDR_TAG);
        String->Length = 0;
        String->MaximumLength = 0;
        String->Buffer = NULL;
    }
}

VOID
FxLdrAcquireLoadedModuleLock()
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Resource, TRUE);
}

VOID
FxLdrReleaseLoadedModuleLock()
{
    ExReleaseResourceLite(&Resource);
    KeLeaveCriticalRegion();
}

NTSTATUS
NTAPI
ConvertUlongToWString(
    _In_ ULONG Value,
    _Inout_ PUNICODE_STRING String)
{
    return RtlIntegerToUnicodeString(Value, 10, String);
}


NTSTATUS
NTAPI
BuildServicePath(
    _In_ PKEY_VALUE_PARTIAL_INFORMATION KeyValueInformation,
    _In_ ULONG Tag,
    _In_ PUNICODE_STRING ServicePath)
{
    NTSTATUS status;
    PWCHAR buffer;
    PWCHAR lastSymbol;
    UNICODE_STRING name;
    CONST WCHAR regPath[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\%wZ";


    if (KeyValueInformation->Type != REG_SZ &&
        KeyValueInformation->Type != REG_EXPAND_SZ) 
    {
        status = STATUS_OBJECT_TYPE_MISMATCH;
        goto error;
    }

    if (KeyValueInformation->DataLength == 0 ||
        KeyValueInformation->DataLength > 0xFFFF) 
    {
        status = STATUS_INVALID_PARAMETER;
        goto error;
    }

    name.Buffer = (PWCH)KeyValueInformation->Data;
    name.Length = (USHORT)KeyValueInformation->DataLength;
    name.MaximumLength = (USHORT)KeyValueInformation->DataLength;

    lastSymbol = ((wchar_t*)KeyValueInformation->Data) + KeyValueInformation->DataLength / 2;
    if (KeyValueInformation->DataLength >= 2 &&    *lastSymbol == 0) 
    {
        name.Length = (USHORT)KeyValueInformation->DataLength - 2;
    }
        
    buffer = ExAllocatePoolWithTag(PagedPool, name.Length + sizeof(regPath), Tag);

    if (buffer != NULL)
    {
        ServicePath->Length = 0;
        ServicePath->MaximumLength = name.Length + sizeof(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");//106;
        ServicePath->Buffer = buffer;
        RtlZeroMemory(ServicePath->Buffer, ServicePath->MaximumLength);
        status = RtlUnicodeStringPrintf(ServicePath, regPath, &name);

        if (!NT_SUCCESS(status)) 
        {
            ExFreePoolWithTag(buffer, WDFLDR_TAG);
            ServicePath->Length = 0;
            ServicePath->Buffer = NULL;
        }
    }
    else 
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        __DBGPRINT(("ERROR: ExAllocatePoolWithTag failed with Status 0x%x\n", status));
    }

    goto done;

error:
    __DBGPRINT(("ERROR: BuildServicePath failed with status 0x%x\n", status));

done:

    return status;
}


VOID
NTAPI
GetNameFromUnicodePath(
    _In_ PUNICODE_STRING Path,
    _Inout_ PWCHAR Dest,
    _In_ LONG DestSize)
{
    PWCHAR stringEnd;
    PWCHAR current;
    NTSTATUS status;

    *Dest = UNICODE_NULL;
    if (Path->Length == 0)
    {
        return;
    }

    stringEnd = &Path->Buffer[Path->Length / sizeof(WCHAR)];

    for (current = stringEnd - 1; *current != '\\'; --current) 
    {
        if (current == Path->Buffer)
            return;
    }

    status = RtlStringCchCopyNW(Dest, DestSize, current + 1, stringEnd - (current + 1));

    if (!NT_SUCCESS(status)) 
    {
        *Dest = L'\0';
    }
}

/*
 * PROJECT:     ReactOS WdfLdr driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Wdf driver intialization
 * COPYRIGHT:   Copyright 2021 Max Korostil (mrmks04@yandex.ru)
 */


#include <ntddk.h>
#include <windef.h>
#include "wdf.h"
#include "../wdfldr/wdfldr.h"


const WDFFUNC *WdfFunctions;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
WDF_BIND_INFO BindInfo = {sizeof(WDF_BIND_INFO), L"KmdfLibrary", {1, 17, 7600}, WdfFunctionTableNumEntries, (void(__fastcall**)())(&WdfFunctions), NULL};
PDRIVER_UNLOAD pOriginalUnload;
DECLARE_UNICODE_STRING_SIZE(gRegistryPath, MAX_PATH);

extern
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath);

VOID
NTAPI
FxDriverUnloadCommon()
{
    WdfVersionUnbind(&gRegistryPath, &BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals);
}

VOID
NTAPI
FxDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    if (pOriginalUnload != NULL)
    {
        pOriginalUnload(DriverObject);
    }
    FxDriverUnloadCommon();
}

NTSTATUS
NTAPI
FxDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status = STATUS_SUCCESS;
    pOriginalUnload = NULL;

    if (DriverObject == NULL)
    {
        return DriverEntry(DriverObject, RegistryPath);
    }

    RtlCopyUnicodeString(&gRegistryPath, RegistryPath);

    status = WdfVersionBind(DriverObject, RegistryPath, &BindInfo, &((PWDF_COMPONENT_GLOBALS)WdfDriverGlobals));
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = DriverEntry(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status))
    {
        FxDriverUnloadCommon();
        return status;
    }

    if (WdfDriverGlobals->DisplaceDriverUnload)
    {
        if (DriverObject->DriverUnload != NULL)
        {
            pOriginalUnload = DriverObject->DriverUnload;
        }
        DriverObject->DriverUnload = FxDriverUnload;
    }
    
    return STATUS_SUCCESS;
}

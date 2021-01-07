/*
 * PROJECT:     ReactOS WdfLdr driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WdfLdr driver - common functions and types
 * COPYRIGHT:   Copyright 2019 mrmks04 (mrmks04@yandex.ru)
 */


#ifndef _GLOBALS_H_
#define _GLOBALS_H_

#include <ntddk.h>
#include <wdm.h>

extern BOOLEAN WdfLdrDiags;
extern LONG gKlibInitialized;
extern ERESOURCE Resource;
extern LIST_ENTRY gLibList;
extern PRtlQueryModuleInformation pfnRtlQueryModuleInformation;

#endif //_GLOBALS_H_

#pragma once

#include <ntdef.h>

typedef struct _RTL_MODULE_BASIC_INFO {
    PVOID ImageBase;
} RTL_MODULE_BASIC_INFO, *PRTL_MODULE_BASIC_INFO;


typedef struct _RTL_MODULE_EXTENDED_INFO {
    RTL_MODULE_BASIC_INFO BasicInfo;
    ULONG                 ImageSize;
    USHORT                FileNameOffset;
    CHAR                  FullPathName[0x100];
} RTL_MODULE_EXTENDED_INFO, *PRTL_MODULE_EXTENDED_INFO;

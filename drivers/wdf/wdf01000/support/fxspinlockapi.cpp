#include "common/fxglobals.h"
#include "common/fxspinlock.h"
#include "common/fxvalidatefunctions.h"


extern "C" {

_Must_inspect_result_
__drv_maxIRQL(DISPATCH_LEVEL)
NTSTATUS
WDFEXPORT(WdfSpinLockCreate)(
    __in
    PWDF_DRIVER_GLOBALS DriverGlobals,
    __in_opt
    PWDF_OBJECT_ATTRIBUTES SpinLockAttributes,
    __out
    WDFSPINLOCK* SpinLock
    )
{
    DDI_ENTRY();

    PFX_DRIVER_GLOBALS pFxDriverGlobals;
    NTSTATUS status;
    FxSpinLock* pLock;
    WDFSPINLOCK lock;
    USHORT extra;

    pFxDriverGlobals = GetFxDriverGlobals(DriverGlobals);

    //
    // Get the parent's globals if it is present
    //
    if (NT_SUCCESS(FxValidateObjectAttributesForParentHandle(
                        pFxDriverGlobals, SpinLockAttributes)))
    {
        FxObject* pParent;

        FxObjectHandleGetPtrAndGlobals(pFxDriverGlobals,
                                       SpinLockAttributes->ParentObject,
                                       FX_TYPE_OBJECT,
                                       (PVOID*)&pParent,
                                       &pFxDriverGlobals);
    }

    FxPointerNotNull(pFxDriverGlobals, SpinLock);

    status = FxValidateObjectAttributes(pFxDriverGlobals, SpinLockAttributes);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (pFxDriverGlobals->FxVerifierLock)
    {
        extra = sizeof(FX_SPIN_LOCK_HISTORY);
    }
    else
    {
        extra = 0;
    }

    *SpinLock = NULL;

    pLock = new(pFxDriverGlobals, SpinLockAttributes, extra)
        FxSpinLock(pFxDriverGlobals, extra);

    if (pLock == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = pLock->Commit(SpinLockAttributes, (WDFOBJECT*)&lock);

    if (NT_SUCCESS(status))
    {
        *SpinLock = lock;
    }
    else
    {
        pLock->DeleteFromFailedCreate();
    }

    return status;
}

} //exter "C"
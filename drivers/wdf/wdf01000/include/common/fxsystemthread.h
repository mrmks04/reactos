#ifndef _FXSYSTEMTHREAD_H_
#define _FXSYSTEMTHREAD_H_

#include "common/fxnonpagedobject.h"
#include "common/mxevent.h"


class FxSystemThread : public FxNonPagedObject {

private:
    MxEvent m_InitEvent;
    MxEvent m_WorkEvent;

    LIST_ENTRY m_WorkList;

    PVOID m_ThreadPtr;

    MdEThread m_PEThread;

    // Used for Async thread spinup and reaping
    //WORK_QUEUE_ITEM m_Spinup; // Async-Thread-Spinup
    WORK_QUEUE_ITEM m_Reaper;

    BOOLEAN m_Exit;
    BOOLEAN m_Initialized;

    FxSystemThread(
        __in PFX_DRIVER_GLOBALS FxDriverGlobals
        );

    //
    // Create the system thread in order to be able to service work items
    //
    // It is recommended this is done from the system process context
    // since the thread's handle is available to the user mode process
    // for a temporary window. XP and later supports OBJ_KERNELHANDLE, but
    // DriverFrameworks must support W2K with the same binary.
    //
    // It is safe to call this at DriverEntry which is in the system process
    // to create an initial driver thread, and this driver thread should be
    // used for creating any child driver threads on demand by using
    // Initialize(FxSystemThread* SpinupThread).
    //
    BOOLEAN
    Initialize(
        VOID
        );

    //
    // This is the thread's main processing function
    //
    VOID
    Thread(
        VOID
        );
        
    //
    // This is the reaper method
    //
    VOID
    Reaper(
        VOID
        );

    _Must_inspect_result_
    NTSTATUS
    CreateThread(
        VOID
        );

    //
    // This is the thunk used to get from the system
    // thread start callback into this class
    //
    static
    VOID
    StaticThreadThunk(
        __inout PVOID Context
        );

public:

    //
    // This is called to tell the thread to exit.
    //
    // It must be called from thread context such as
    // the driver unload routine since it will wait for the
    // thread to exit.
    //
    // A worker thread will not exit unless it has processed
    // all of its queued work items. If processing of queued
    // workitems is no longer desired, then use CancelWorkItem
    // to remove the items before calling this method.
    //
    BOOLEAN
    ExitThread(
        VOID
        );

    virtual
    ~FxSystemThread(
        VOID
        );

    //
    // Determine if current thread is this
    // worker thread
    //
    __inline
    BOOLEAN
    IsCurrentThread()
    {
        return (Mx::GetCurrentEThread() == m_PEThread) ? TRUE : FALSE;
    }

    DECLARE_INTERNAL_NEW_OPERATOR();

    //
    // Queue a work item to the thread
    //
    // It is valid to queue work items before thread
    // Initialize()/Initialize(FxSystemThread*) is called. The items
    // remain queued until the system thread is started.
    //
    BOOLEAN
    QueueWorkItem(
        __inout PWORK_QUEUE_ITEM WorkItem
        );

    _Must_inspect_result_
    static
    NTSTATUS
    _CreateAndInit(
        __deref_out FxSystemThread** SystemThread,
        __in PFX_DRIVER_GLOBALS FxDriverGlobals,
        __in WDFDEVICE Device,
        __in MdDeviceObject DeviceObject
        );
};

#endif //_FXSYSTEMTHREAD_H_
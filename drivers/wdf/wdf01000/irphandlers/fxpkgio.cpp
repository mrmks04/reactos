#include "common/fxpkgio.h"
#include "common/fxdevice.h"
#include "common/fxverifier.h"
#include "common/fxirpdynamicdispatchinfo.h"
#include "common/fxglobals.h"
#include "common/fxverifier.h"


//
// This package is initialized by the FxPkgIo::Install virtual method
// being invoked.
//
// A reference is held on it by the FxDevice which owns it. When the
// FxDevice is destroyed, its destructor FxDevice::~FxDevice will release
// its reference to this package, so that FxPkgIo::~FxPkgIo can run.
//
// There is no other package remove, or un-install call.
//

FxPkgIo::FxPkgIo(
    __in PFX_DRIVER_GLOBALS FxDriverGlobals,
    __in CfxDevice *Device
    ) :
    FxPackage(FxDriverGlobals, Device, FX_TYPE_PACKAGE_IO),
    m_InCallerContextCallback(FxDriverGlobals)
{
    LARGE_INTEGER  tickCount;

    m_Device = Device;

    m_DefaultQueue = NULL;

    RtlZeroMemory(m_DispatchTable, sizeof(m_DispatchTable));

    m_Filter = FALSE;

    m_PowerStateOn = FALSE;

    m_QueuesAreShuttingDown = FALSE;
    
    InitializeListHead(&m_IoQueueListHead);
    
    InitializeListHead(&m_DynamicDispatchInfoListHead);

    Mx::MxQueryTickCount(&tickCount);

    m_RandomSeed = tickCount.LowPart;
 
    DoTraceLevelMessage(FxDriverGlobals, TRACE_LEVEL_VERBOSE, TRACINGIO,
                        "Constructed FxPkgIo 0x%p",this);
}

FxPkgIo::~FxPkgIo()
{
    PLIST_ENTRY next;

    m_DefaultQueue = NULL;

    m_Device = NULL;
    
    while (!IsListEmpty(&m_DynamicDispatchInfoListHead))
    {
        next = RemoveHeadList(&m_DynamicDispatchInfoListHead);
        FxIrpDynamicDispatchInfo* info;
        info = CONTAINING_RECORD(next, FxIrpDynamicDispatchInfo, ListEntry);
        InitializeListHead(next);
        delete info;
    }

    ASSERT(IsListEmpty(&m_IoQueueListHead));

    DoTraceLevelMessage(GetDriverGlobals(), TRACE_LEVEL_VERBOSE, TRACINGIO,
                        "Destroyed FxPkgIo 0x%p",this);
}

_Must_inspect_result_
NTSTATUS
FxPkgIo::Dispatch(
    __inout MdIrp Irp
    )
{
    FxIrp fxIrp(Irp);
    FX_TRACK_DRIVER(GetDriverGlobals());
    
    DoTraceLevelMessage(
        GetDriverGlobals(), TRACE_LEVEL_VERBOSE, TRACINGIO,
        "WDFDEVICE 0x%p !devobj 0x%p %!IRPMJ!, IRP_MN %x, IRP 0x%p",
        m_Device->GetHandle(), m_Device->GetDeviceObject(),
        fxIrp.GetMajorFunction(),
        fxIrp.GetMinorFunction(), Irp);
    
    return DispatchStep1(Irp, m_DynamicDispatchInfoListHead.Flink);
}

_Must_inspect_result_
NTSTATUS
FxPkgIo::SetFilter(
    __in BOOLEAN Value
    )
{
    PFX_DRIVER_GLOBALS FxDriverGlobals = GetDriverGlobals();

    if (m_DefaultQueue != NULL)
    {
        DoTraceLevelMessage(FxDriverGlobals, TRACE_LEVEL_ERROR, TRACINGIO,
                            "I/O Package already has a default queue. "
                            "SetFilter must be called before creating "
                            "a default queue %!STATUS!",
                            STATUS_INVALID_DEVICE_REQUEST);

        FxVerifierDbgBreakPoint(FxDriverGlobals);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    m_Filter = Value;

    return STATUS_SUCCESS;
}

__inline
_Must_inspect_result_
NTSTATUS
__fastcall
FxPkgIo::DispatchStep1(
    __inout MdIrp       Irp,
    __in    WDFCONTEXT  DispatchContext
    )
/*++

    Routine Description:

    Checks for any registered dynamic dispatch callbacks that handles this type of request, else
    selects the default queue based on the IRP's major code. 

Arguments:

    Irp - WDM request.

    DispatchContext -  Is the next FxIrpDynamicDispatchInfo element.

Return Value:

    Irp's status.

--*/

{       
    NTSTATUS                status;
    FxIrp                   fxIrp(Irp);

    ASSERT(((UCHAR)DispatchContext & FX_IN_DISPATCH_CALLBACK) == 0);
    
    ASSERT(fxIrp.GetMajorFunction() <= IRP_MJ_MAXIMUM_FUNCTION);
    
    //
    // Look for I/O dynamic dispatch callbacks.
    //
    if ((PLIST_ENTRY)DispatchContext != &m_DynamicDispatchInfoListHead)
    {
        int     index;
        index = FxIrpDynamicDispatchInfo::Mj2Index(fxIrp.GetMajorFunction());
        
        //
        // Only read/writes/ctrls/internal_ctrls IRPs are allowed, i.e., request cannot 
        // IRP type in its callback.
        //
        if (index >= (int)FxIrpDynamicDispatchInfo::DynamicDispatchMax)
        {
            status = STATUS_INVALID_PARAMETER;
            DoTraceLevelMessage(
                    GetDriverGlobals(), TRACE_LEVEL_ERROR, TRACINGIO,
                    "Driver cannot change the IRP type in its dispatch "
                    "callback Irp 0x%p, %!IRPMJ!, IRP_MN %x, Device 0x%p, "
                    "%!STATUS!",
                    Irp, fxIrp.GetMajorFunction(), fxIrp.GetMinorFunction(), 
                    m_Device->GetHandle(), status);

            FxVerifierDbgBreakPoint(GetDriverGlobals());
            goto CompleteIrp;
        }
        
        //
        // Verifier checks.
        //
        status = VerifyDispatchContext(GetDriverGlobals(), DispatchContext);
        if( !NT_SUCCESS(status))
        {
            goto CompleteIrp;
        }

        do
        {
            FxIrpDynamicDispatchInfo* info;
                
            info = CONTAINING_RECORD(DispatchContext, 
                                     FxIrpDynamicDispatchInfo, 
                                     ListEntry);
            //
            // Advance to next node.
            //
            DispatchContext = (WDFCONTEXT)(((PLIST_ENTRY)DispatchContext)->Flink);
            ASSERT(((UCHAR)DispatchContext & FX_IN_DISPATCH_CALLBACK) == 0);
            
            ASSERT(fxIrp.GetMajorFunction() == IRP_MJ_READ ||
                   fxIrp.GetMajorFunction() == IRP_MJ_WRITE ||
                   fxIrp.GetMajorFunction() == IRP_MJ_DEVICE_CONTROL ||
                   fxIrp.GetMajorFunction() == IRP_MJ_INTERNAL_DEVICE_CONTROL);
            
            //
            // If registered, invoke dispatch callback for this major function.
            //
            ASSERT(index < (int)FxIrpDynamicDispatchInfo::DynamicDispatchMax);
            /*if (NULL != info->Dispatch[index].EvtDeviceDynamicDispatch)
            {
                return info->Dispatch[index].EvtDeviceDynamicDispatch(
                                m_Device->GetHandle(), 
                                fxIrp.GetMajorFunction(),
                                fxIrp.GetMinorFunction(),
                                fxIrp.GetParameterIoctlCode(),
                                info->Dispatch[index].DriverContext,
                                reinterpret_cast<PIRP> (fxIrp.GetIrp()),
                                (WDFCONTEXT)((ULONG_PTR)DispatchContext | 
                                              FX_IN_DISPATCH_CALLBACK)
                                );
            }*/
         } while ((PLIST_ENTRY)DispatchContext != 
                                &m_DynamicDispatchInfoListHead);
    }

    //
    // Only now push these local variables on the stack, this is to keep the 
    // stack from growing unnecessarily in the dynamic dispatch path above.
    //
    FxIoQueue*              queue;
    FxIoInCallerContext*    ioInCallerCtx;

    //
    // Get the queue from the dispatch-table
    //
    queue = m_DispatchTable[fxIrp.GetMajorFunction()];
    if (queue == NULL)
    {
        ioInCallerCtx = GetIoInCallerContextCallback(NULL);
        if (ioInCallerCtx->m_Method == NULL)
        {
            //
            // No queue configured yet, fail request unless the driver is a filter.
            //
            if (m_Filter) {
                goto Forward;
            }
        
            status = STATUS_INVALID_DEVICE_REQUEST;
            DoTraceLevelMessage(
                GetDriverGlobals(), TRACE_LEVEL_ERROR, TRACINGIO,
                "No queue configured for WDFDEVICE 0x%p, failing IRP 0x%p,"
                " %!STATUS!",
                m_Device->GetHandle(), Irp, status);
        
            goto CompleteIrp;
        }
    }
    else
    {
        ioInCallerCtx = GetIoInCallerContextCallback(queue->GetCxDeviceInfo());
    }

    //
    // If the driver is filter and queue is a default-queue then before
    // calling the queue, we should make sure the queue can dispatch
    // requests to the driver. If the queue cannot dispatch request,
    // we should forward it down to the lower driver ourself.
    // This is to cover the scenario where the driver has registered only
    // type specific handler and expect the framework to auto-forward other
    // requests.
    //
    if (m_Filter &&
        ioInCallerCtx->m_Method == NULL &&
        queue == m_DefaultQueue &&
        queue->IsIoEventHandlerRegistered((WDF_REQUEST_TYPE)fxIrp.GetMajorFunction()) == FALSE)
    {
        //
        // Default queue doesn't have callback events registered to
        // handle this request. So forward it down.
        //
        goto Forward;
    }

    //
    // Finally queue request.
    //
    return DispatchStep2(Irp, ioInCallerCtx, queue);

Forward:

    fxIrp.SkipCurrentIrpStackLocation();
    return fxIrp.CallDriver(m_Device->GetAttachedDevice());

CompleteIrp:

    fxIrp.SetStatus(status);
    fxIrp.SetInformation(0);
    fxIrp.CompleteRequest(IO_NO_INCREMENT);

    return status;
}

_Must_inspect_result_
NTSTATUS
FX_VF_METHOD(FxPkgIo, VerifyDispatchContext) (
    _In_ PFX_DRIVER_GLOBALS FxDriverGlobals,
    _In_ WDFCONTEXT DispatchContext
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN ctxValid;
    PLIST_ENTRY next;

    PAGED_CODE_LOCKED();
    
    //
    // Make sure context is valid.
    //
    ctxValid = (PLIST_ENTRY)DispatchContext == 
                    &m_DynamicDispatchInfoListHead ? 
                        TRUE : FALSE;

    for (next = m_DynamicDispatchInfoListHead.Flink;
         next != &m_DynamicDispatchInfoListHead;
         next = next->Flink)
    {
        if ((PLIST_ENTRY)DispatchContext == next)
        {
            ctxValid = TRUE;
            break;
        }
    }

    if (FALSE == ctxValid)
    {
        status = STATUS_INVALID_PARAMETER;
        DoTraceLevelMessage(
                FxDriverGlobals, TRACE_LEVEL_ERROR, TRACINGIO,
                "DispatchContext 0x%p is invalid, %!STATUS!",
                DispatchContext, status);
        FxVerifierDbgBreakPoint(FxDriverGlobals);
    }
    
    return status;
}

__inline
_Must_inspect_result_
NTSTATUS
__fastcall
FxPkgIo::DispatchStep2(
    __inout  MdIrp       Irp,
    __in_opt FxIoInCallerContext* IoInCallerCtx,
    __in_opt FxIoQueue*  Queue
    )
{       
    NTSTATUS            status;
    FxRequest*          request;
    BOOLEAN             isForwardProgressQueue;
    BOOLEAN             inCriticalRegion;
    PWDF_OBJECT_ATTRIBUTES reqAttribs;
    FxIrp               fxIrp(Irp);
    
    request = NULL;
    inCriticalRegion = FALSE;
    isForwardProgressQueue = Queue != NULL && Queue->IsForwardProgressQueue();

    ASSERT(fxIrp.GetMajorFunction() <= IRP_MJ_MAXIMUM_FUNCTION);
    ASSERT((IoInCallerCtx != NULL && IoInCallerCtx->m_Method != NULL) ||
            Queue != NULL);
    //
    // The request inserted into the queue can be retrieved and processed 
    // by an arbitrary thread. So we need to make sure that such a thread doesn't
    // get suspended and deadlock the driver and potentially the system by 
    // entering critical region.The KeEnterCriticalRegion temporarily disables 
    // the delivery of normal kernel APCs used to suspend a thread. 
    // Kernel APCs queued to this thread will get executed when we leave the 
    // critical region. 
    //
    if (Mx::MxGetCurrentIrql() <= APC_LEVEL)
    {
        Mx::MxEnterCriticalRegion();
        inCriticalRegion = TRUE;
    }

    if (Queue != NULL && Queue->GetCxDeviceInfo() != NULL)
    {
        reqAttribs = &Queue->GetCxDeviceInfo()->RequestAttributes;
    }
    else
    {
        reqAttribs = m_Device->GetRequestAttributes(); 
    }

    status = FxRequest::_CreateForPackage(m_Device, reqAttribs, Irp, &request);
    
    //
    // Check if it is forward progress queue and the EnhancedVerifierOption for
    // testing forward progress are set. 
    //
    if (isForwardProgressQueue && 
        NT_SUCCESS(status) &&
        IsFxVerifierTestForwardProgress(GetDriverGlobals()))
    {
        //
        // This function returns STATUS_INSUFFICIENT_RESOURCES 
        // if testing forward progress is enabled and free's the passed in request.
        //
        status = VerifierFreeRequestToTestForwardProgess(request);
    }

    if (!NT_SUCCESS(status))
    {
        if (m_Filter && Queue == NULL)
        {
           goto CompleteIrp;
        }

        if (isForwardProgressQueue)
        {
            status = Queue->GetReservedRequest(Irp, &request);
            if (status == STATUS_PENDING)
            {
                goto IrpIsGone;
            }
            else if (!NT_SUCCESS(status))
            {
                goto CompleteIrp;
            }
        }
        else
        {
            //
            // Fail the request
            //
            DoTraceLevelMessage(
                GetDriverGlobals(), TRACE_LEVEL_ERROR, TRACINGIO,
                "Could not create WDFREQUEST, %!STATUS!", status);

            goto CompleteIrp;
        }
    }
    else
    {
        if (isForwardProgressQueue)
        {
            status = Queue->InvokeAllocateResourcesCallback(request); 
            if (!NT_SUCCESS(status))
            {
                //
                // Failure of the callback means the driver wasn't able to 
                // allocate resources for the request. In that case free the 
                // request allocated earlier and use the reserved one.
                // 
                request->FreeRequest();
                request = NULL;
                
                status = Queue->GetReservedRequest(Irp, &request);
                if (status == STATUS_PENDING)
                {
                    goto IrpIsGone;
                }
                else if (!NT_SUCCESS(status))
                {
                    goto CompleteIrp;
                }            
            }
        }
    }

    //
    // Since we can't guarantee the callback to be called in the context of the
    // caller for reserved requests, we will skip calling InCallerContextCallback
    // for reserverd request. 
    //
    if (IoInCallerCtx != NULL &&
        IoInCallerCtx->m_Method != NULL && 
        request->IsReserved() == FALSE)
    {        
        request->SetInternalContext(Queue);
        status = DispathToInCallerContextCallback(IoInCallerCtx, request, Irp);

        //
        // The driver is responsible for calling WdfDeviceEnqueueRequest to 
        // insert it back into the I/O processing pipeline, or completing it.
        //
        goto IrpIsGone;
    }

    ASSERT(Queue != NULL);
    status = Queue->QueueRequest(request);
    goto IrpIsGone;

CompleteIrp:

    fxIrp.SetStatus(status);
    fxIrp.SetInformation(0);
    fxIrp.CompleteRequest(IO_NO_INCREMENT);
    //
    // fallthrough
    //
IrpIsGone:

    if (inCriticalRegion)
    {
        Mx::MxLeaveCriticalRegion();
    }
    
    return status;
}

_Must_inspect_result_
NTSTATUS
FxPkgIo::VerifierFreeRequestToTestForwardProgess(
    __in FxRequest* Request
    )
{
    BOOLEAN failAllocation;
    PFX_DRIVER_GLOBALS pFxDriverGlobals;

    pFxDriverGlobals = GetDriverGlobals();    
    failAllocation = FALSE;
    //
    // forwardProgressTestFailAll takes precedence over forwardProgressTestFailRandom
    //
    if (IsFxVerifierTestForwardProgressFailAll(pFxDriverGlobals))
    {       
        failAllocation = TRUE;
    } 
    else if (IsFxVerifierTestForwardProgressFailRandom(pFxDriverGlobals))
    {
        //
        // Modulo 17 makes the probability of failure ~6% just like verifier.exe
        //
        failAllocation = (FxRandom(&m_RandomSeed) % 17 == 0);
    }
    
    if (failAllocation)
    {
        //
        // Don't use DeleteObject() here as the Request wasn't presented to the 
        // driver and the cleanup /dispose callback can be invoked unless 
        // we use FreeRequest() which clears the cleanup /dispose callbacks
        //
        Request->FreeRequest();
    
        return STATUS_INSUFFICIENT_RESOURCES;
    } 
    else
    {
        return STATUS_SUCCESS;
    }
}

NTSTATUS
FxPkgIo::DispathToInCallerContextCallback(
    __in    FxIoInCallerContext *InCallerContextInfo,
    __in    FxRequest *Request,
    __inout MdIrp      Irp
    )
{
    PFX_DRIVER_GLOBALS pFxDriverGlobals;
    FxRequestCompletionState oldState;
    FxIrp fxIrp(Irp);

    pFxDriverGlobals = GetDriverGlobals();
            
    //
    // Mark the IRP pending since we are going
    // to return STATUS_PENDING regardless of whether
    // the driver completes the request right away
    // or not
    //
    fxIrp.MarkIrpPending();

    if (pFxDriverGlobals->FxVerifierOn)
    {
        Request->SetVerifierFlags(FXREQUEST_FLAG_DRIVER_INPROCESS_CONTEXT |
                                  FXREQUEST_FLAG_DRIVER_OWNED);
    }

    //
    // Set a completion callback to manage the reference
    // count on the request
    //
    oldState = Request->SetCompletionState(FxRequestCompletionStateIoPkg);

    ASSERT(oldState == FxRequestCompletionStateNone);
    UNREFERENCED_PARAMETER(oldState);
    
    //
    // Release the reference count on the request since
    // the callback will hold the only one that gets
    // decremented when the request is completed
    //
    Request->RELEASE(FXREQUEST_STATE_TAG);

    Request->SetPresented();
    
    //
    // Drivers that use this API are responsible for handling
    // all locking, threading, and IRQL level issues...
    //
    InCallerContextInfo->Invoke(m_Device->GetHandle(), 
                                Request->GetHandle());
    //
    // The driver is responsible for calling WdfDeviceEnqueueRequest to insert
    // it back into the I/O processing pipeline, or completing it.
    //
    
    return STATUS_PENDING;
}

_Must_inspect_result_
NTSTATUS
FxPkgIo::CreateQueue(
    __in  PWDF_IO_QUEUE_CONFIG     Config,
    __in  PWDF_OBJECT_ATTRIBUTES   QueueAttributes,
    __in_opt FxDriver*             Caller,
    __deref_out FxIoQueue**        ppQueue
    )
{
    PFX_DRIVER_GLOBALS pFxDriverGlobals;
    FxObject* pParent;
    FxIoQueue* pQueue;
    NTSTATUS status;
    FxDriver* pDriver;

    pParent = NULL;
    pQueue = NULL;
    pDriver = NULL;
    pFxDriverGlobals = GetDriverGlobals();

    if (QueueAttributes != NULL && QueueAttributes->ParentObject != NULL)
    {
        CfxDeviceBase* pSearchDevice;

        FxObjectHandleGetPtr(pFxDriverGlobals,
                             QueueAttributes->ParentObject,
                             FX_TYPE_OBJECT,
                             (PVOID*)&pParent);

        pSearchDevice = FxDeviceBase::_SearchForDevice(pParent, NULL);

        if (pSearchDevice == NULL)
        {
            status = STATUS_INVALID_DEVICE_REQUEST;

            DoTraceLevelMessage(
                pFxDriverGlobals, TRACE_LEVEL_ERROR, TRACINGIO,
                "QueueAttributes->ParentObject 0x%p must have WDFDEVICE as an "
                "eventual ancestor, %!STATUS!",
                QueueAttributes->ParentObject, status);

            return status;
        }
        else if (pSearchDevice != m_DeviceBase)
        {
            status = STATUS_INVALID_DEVICE_REQUEST;

            DoTraceLevelMessage(
                pFxDriverGlobals, TRACE_LEVEL_ERROR, TRACINGIO,
                "Attributes->ParentObject 0x%p ancestor is WDFDEVICE %p, but "
                "not the same WDFDEVICE 0x%p passed to WdfIoQueueCreate, "
                "%!STATUS!",
                QueueAttributes->ParentObject, pSearchDevice->GetHandle(),
                m_Device->GetHandle(), status);

            return status;
        }
    }
    else
    {
        //
        // By default, use the package as the parent
        //
        pParent = this;
    }

    //
    // v1.11 and up: get driver object if driver handle is specified.
    // Client driver can also specify a driver handle, the end result in this case is 
    // a PkgIoContext that is NULL (see below), i.e., the same as if driver handle 
    // was NULL.
    //
    /*if (Config->Size > sizeof(WDF_IO_QUEUE_CONFIG_V1_9) && 
        Config->Driver != NULL)
    {

        FxObjectHandleGetPtr(GetDriverGlobals(),
                             Config->Driver,
                             FX_TYPE_DRIVER,
                             (PVOID*)&pDriver);
    }*/
    
    status = FxIoQueue::_Create(pFxDriverGlobals,
                            QueueAttributes,
                            Config,
                            Caller,
                            this,
                            m_PowerStateOn,
                            &pQueue
                            );

    if (!NT_SUCCESS(status))
    {
        ASSERT(pQueue == NULL);
        return status;
    }

    //
    // Class extension support: associate queue with a specific cx layer.
    //
    if (pDriver != NULL)
    {
        pQueue->SetCxDeviceInfo(m_Device->GetCxDeviceInfo(pDriver));
    }
   
    status = pQueue->Commit(QueueAttributes, NULL, pParent);
    if (!NT_SUCCESS(status))
    {
       pQueue->DeleteFromFailedCreate();
       return status;
    }

    AddIoQueue(pQueue);
    *ppQueue = pQueue;

    return status;
}

VOID
FxPkgIo::AddIoQueue(
    __inout FxIoQueue* IoQueue
    )
{
    PLIST_ENTRY         listHead, le;
    FxIoQueue*          queue;
    KIRQL               irql;
    FxIoQueueNode*      listNode;
    CCHAR               queueIndex, curIndex;
    
    listHead = &m_IoQueueListHead;
    queueIndex = FxDevice::GetCxDriverIndex(IoQueue->GetCxDeviceInfo());
    Lock(&irql);

    ASSERT(IoQueue->m_IoPkgListNode.IsNodeType(FxIoQueueNodeTypeQueue));

    //
    // Insert new queue in sorted list; search from last to first.
    //
    for (le = listHead->Blink; le != listHead; le = le->Blink)
    {
        //
        // Skip any nodes that are not queues. They can be bookmarks for
        // in-progress flush operations.
        //
        listNode = FxIoQueueNode::_FromListEntry(le);
        if (listNode->IsNodeType(FxIoQueueNodeTypeQueue) == FALSE)
        {
            continue;
        }

        //
        // Get current queue's driver index.
        //
        queue = FxIoQueue::_FromIoPkgListEntry(le);
        curIndex = FxDevice::GetCxDriverIndex(queue->GetCxDeviceInfo());
        // 
        // Queues are inserted in order at the end of its allowed range.
        //
        if (curIndex < queueIndex || curIndex == queueIndex)
        {
            break;
        }
    }
        
    InsertHeadList(le, &IoQueue->m_IoPkgListNode.m_ListEntry);
    
    if (m_PowerStateOn)
    {
        IoQueue->SetPowerState(FxIoQueuePowerOn);
    }
    else
    {
        IoQueue->SetPowerState(FxIoQueuePowerOff);
        if (m_QueuesAreShuttingDown)
        {
            // Clear accept requests
            IoQueue->SetStateForShutdown();
        }
    }

    Unlock(irql);

    return;
}

_Must_inspect_result_
NTSTATUS
FxPkgIo::FlushAllQueuesByFileObject(
    __in MdFileObject FileObject
    )

/*++

    Routine Description:

        Enumerate all the queues and cancel the requests that have
        the same fileobject as the Cleanup IRP.

        We are making an assumption that cleanup irps are sent only
        at passive-level.

    Return Value:

    NTSTATUS

--*/
{
    FxIoQueue* queue = NULL;
    PFX_DRIVER_GLOBALS pFxDriverGlobals = GetDriverGlobals();
    FxIoQueueNode flushBookmark(FxIoQueueNodeTypeBookmark);
    KIRQL irql;
    
    if (Mx::MxGetCurrentIrql() != PASSIVE_LEVEL)
    {
        DoTraceLevelMessage(pFxDriverGlobals, TRACE_LEVEL_ERROR, TRACINGIO,
                        "Currently framework allow flushing of queues "
                        "by fileobject on cleanup only at PASSIVE_LEVEL");

        FxVerifierDbgBreakPoint(pFxDriverGlobals);
        return STATUS_SUCCESS;
    }

    //
    // Iterate through the queue list and flush each one.
    //
    Lock(&irql);
    queue = GetFirstIoQueueLocked(&flushBookmark, IO_ITERATOR_FLUSH_TAG);
    Unlock(irql);

    while(queue != NULL)
    {
        queue->FlushByFileObject(FileObject);

        queue->RELEASE(IO_ITERATOR_FLUSH_TAG);

        Lock(&irql);
        queue = GetNextIoQueueLocked(&flushBookmark, IO_ITERATOR_FLUSH_TAG);
        Unlock(irql);
    }

    return STATUS_SUCCESS;
}

FxIoQueue*
FxPkgIo::GetFirstIoQueueLocked(
    __in FxIoQueueNode* QueueBookmark,
    __in PVOID Tag
    )
/*++

    Routine Description:

        Inserts the provided bookmark (FxIoQueueNode) at the beginning 
        of the IO Package's queue list, and calls GetNextIoQueueLocked
        to retrieve the first queue and to advance the bookmark.

        Function is called with the FxPkg lock held.
        
    Return Value:

        NULL            if there are no queues in list else 
        FxIoQueue*      reference to first queue in list.

--*/
{
    ASSERT(QueueBookmark->IsNodeType(FxIoQueueNodeTypeBookmark));
    ASSERT(IsListEmpty(&QueueBookmark->m_ListEntry));
    
    InsertHeadList(&m_IoQueueListHead, &QueueBookmark->m_ListEntry);
    
    return GetNextIoQueueLocked(QueueBookmark, Tag);
}

FxIoQueue*
FxPkgIo::GetNextIoQueueLocked(
    __in FxIoQueueNode* QueueBookmark,
    __in PVOID Tag
    )
/*++

    Routine Description:

        Moves the provided bookmark ahead in the IO Package's queue list
        and returns the next available queue (if any).

    Return Value:

        NULL            if there are no more queues in list else 
        FxIoQueue*      reference to the next queue in list.

--*/
{
    PLIST_ENTRY     ple      = NULL;
    FxIoQueue*      queue    = NULL;
    FxIoQueueNode*  listNode = NULL;

    ASSERT(QueueBookmark->IsNodeType(FxIoQueueNodeTypeBookmark));
    ASSERT(IsListEmpty(&QueueBookmark->m_ListEntry) == FALSE);
    
    //
    // Try to advance bookmark to next location.
    //    
    ple = QueueBookmark->m_ListEntry.Flink;
    RemoveEntryList(&QueueBookmark->m_ListEntry);
    InitializeListHead(&QueueBookmark->m_ListEntry);

    for (; ple != &m_IoQueueListHead; ple = ple->Flink)
    {
        //
        // Skip any nodes that are not queues. These nodes can be 
        // bookmarks for in-progress flush operations.
        //
        listNode = FxIoQueueNode::_FromListEntry(ple);
        if (listNode->IsNodeType(FxIoQueueNodeTypeQueue))
        {
            //
            // This entry is a real queue.
            //
            queue = FxIoQueue::_FromIoPkgListEntry(ple);        
            queue->ADDREF(Tag);

            //
            // Insert bookmark after this entry.
            //
            InsertHeadList(ple, &QueueBookmark->m_ListEntry);

            break;
        }
    }

    return queue;
}

_Must_inspect_result_
NTSTATUS
FxPkgIo::InitializeDefaultQueue(
    __in    CfxDevice               * Device,
    __inout FxIoQueue               * Queue
    )

/*++

    Routine Description:

    Make the input queue as the default queue. There can be
    only one queue as the default queue.

    The default queue is the place all requests go to
    automatically if a specific queue was not configured
    for them.

Arguments:


Return Value:

    NTSTATUS

--*/
{
    PFX_DRIVER_GLOBALS FxDriverGlobals = GetDriverGlobals();
    ULONG index;

    if (m_DefaultQueue != NULL)
    {
        DoTraceLevelMessage(FxDriverGlobals, TRACE_LEVEL_ERROR, TRACINGIO,
                            "Default Queue Already Configured for "
                            "FxPkgIo 0x%p, WDFDEVICE 0x%p %!STATUS!",this,
                            Device->GetHandle(), STATUS_UNSUCCESSFUL);
        return STATUS_UNSUCCESSFUL;
    }

    for (index=0; index <= IRP_MJ_MAXIMUM_FUNCTION; index++)
    {
        if (m_DispatchTable[index] == NULL)
        {
            m_DispatchTable[index] = Queue;
        }
    }

    m_DefaultQueue = Queue;

    //
    // Default queue can't be deleted. So mark the object to fail WdfObjectDelete on
    // the default queue.
    //
    Queue->MarkNoDeleteDDI();
    return STATUS_SUCCESS;
}
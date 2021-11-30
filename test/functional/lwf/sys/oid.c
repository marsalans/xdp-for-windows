//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#include "precomp.h"

typedef struct _XDP_OID_CLONE {
    NDIS_OID_REQUEST *OriginalRequest;
} XDP_OID_CLONE;

C_ASSERT(sizeof(XDP_OID_CLONE) <= RTL_FIELD_SIZE(NDIS_OID_REQUEST, SourceReserved));

typedef struct _XDP_OID_REQUEST {
    NDIS_OID_REQUEST Oid;
    NDIS_STATUS Status;
    KEVENT CompletionEvent;
} XDP_OID_REQUEST;

static
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
OidInternalRequest(
    _In_ NDIS_HANDLE NdisFilterHandle,
    _In_ NDIS_REQUEST_TYPE RequestType,
    _In_ NDIS_OID Oid,
    _Inout_updates_bytes_to_(InformationBufferLength, *BytesProcessed)
        VOID *InformationBuffer,
    _In_ ULONG InformationBufferLength,
    _In_opt_ ULONG OutputBufferLength,
    _In_ ULONG MethodId,
    _Out_ ULONG *BytesProcessed
    )
{
    XDP_OID_REQUEST FilterRequest;
    NDIS_OID_REQUEST *NdisRequest = &FilterRequest.Oid;
    NDIS_STATUS Status;

    *BytesProcessed = 0;
    RtlZeroMemory(NdisRequest, sizeof(NDIS_OID_REQUEST));

    KeInitializeEvent(&FilterRequest.CompletionEvent, NotificationEvent, FALSE);

    NdisRequest->Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    NdisRequest->Header.Revision = NDIS_OID_REQUEST_REVISION_2;
    NdisRequest->Header.Size = sizeof(NDIS_OID_REQUEST);
    NdisRequest->RequestType = RequestType;

    switch (RequestType) {
    case NdisRequestQueryInformation:
        NdisRequest->DATA.QUERY_INFORMATION.Oid = Oid;
        NdisRequest->DATA.QUERY_INFORMATION.InformationBuffer =
            InformationBuffer;
        NdisRequest->DATA.QUERY_INFORMATION.InformationBufferLength =
            InformationBufferLength;
        break;

    case NdisRequestSetInformation:
        NdisRequest->DATA.SET_INFORMATION.Oid = Oid;
        NdisRequest->DATA.SET_INFORMATION.InformationBuffer =
            InformationBuffer;
        NdisRequest->DATA.SET_INFORMATION.InformationBufferLength =
            InformationBufferLength;
        break;

    case NdisRequestMethod:
        NdisRequest->DATA.METHOD_INFORMATION.Oid = Oid;
        NdisRequest->DATA.METHOD_INFORMATION.MethodId = MethodId;
        NdisRequest->DATA.METHOD_INFORMATION.InformationBuffer =
            InformationBuffer;
        NdisRequest->DATA.METHOD_INFORMATION.InputBufferLength =
            InformationBufferLength;
        NdisRequest->DATA.METHOD_INFORMATION.OutputBufferLength =
            OutputBufferLength;
        break;

    default:
        ASSERT(FALSE);
        break;
    }

    NdisRequest->RequestId = (VOID *)OidInternalRequest;

    Status = NdisFOidRequest(NdisFilterHandle, NdisRequest);

    if (Status == NDIS_STATUS_PENDING) {
        KeWaitForSingleObject(
            &FilterRequest.CompletionEvent, Executive, KernelMode, FALSE, NULL);
        Status = FilterRequest.Status;
    }

    if (Status == NDIS_STATUS_INVALID_LENGTH) {
        // Map NDIS status to STATUS_BUFFER_TOO_SMALL.
        Status = NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    if (Status == NDIS_STATUS_SUCCESS) {
        if (RequestType == NdisRequestSetInformation) {
            *BytesProcessed = NdisRequest->DATA.SET_INFORMATION.BytesRead;
        }

        if (RequestType == NdisRequestQueryInformation) {
            *BytesProcessed = NdisRequest->DATA.QUERY_INFORMATION.BytesWritten;
        }

        if (RequestType == NdisRequestMethod) {
            *BytesProcessed = NdisRequest->DATA.METHOD_INFORMATION.BytesWritten;
        }
    } else if (Status == NDIS_STATUS_BUFFER_TOO_SHORT) {
        if (RequestType == NdisRequestSetInformation) {
            *BytesProcessed = NdisRequest->DATA.SET_INFORMATION.BytesNeeded;
        }

        if (RequestType == NdisRequestQueryInformation) {
            *BytesProcessed = NdisRequest->DATA.QUERY_INFORMATION.BytesNeeded;
        }

        if (RequestType == NdisRequestMethod) {
            *BytesProcessed = NdisRequest->DATA.METHOD_INFORMATION.BytesNeeded;
        }
    }

    return XdpConvertNdisStatusToNtStatus(Status);
}

static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
OidInternalRequestComplete(
    _In_ LWF_FILTER *Filter,
    _In_ NDIS_OID_REQUEST *Request,
    _In_ NDIS_STATUS Status
    )
{
    XDP_OID_REQUEST *FilterRequest;

    UNREFERENCED_PARAMETER(Filter);

    FilterRequest = CONTAINING_RECORD(Request, XDP_OID_REQUEST, Oid);
    FilterRequest->Status = Status;
    KeSetEvent(&FilterRequest->CompletionEvent, 0, FALSE);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
OidIrpSubmitRequest(
    _In_ LWF_FILTER *Filter,
    _In_ IRP *Irp,
    _In_ IO_STACK_LOCATION *IrpSp
    )
{
    OID_SUBMIT_REQUEST_IN *In = Irp->AssociatedIrp.SystemBuffer;
    ULONG BytesReturned;
    NTSTATUS Status;
    BOUNCE_BUFFER InfoBuffer;

    BounceInitialize(&InfoBuffer);

    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*In)) {
        Status = STATUS_BUFFER_TOO_SMALL;
        goto Exit;
    }

    Status =
        BounceBuffer(
            &InfoBuffer, In->InformationBuffer, In->InformationBufferLength, __alignof(UCHAR));
    if (!NT_SUCCESS(Status)) {
        goto Exit;
    }

    Status =
        OidInternalRequest(
            Filter->NdisFilterHandle, In->Key.RequestType, In->Key.Oid, InfoBuffer.Buffer,
            In->InformationBufferLength, 0, 0, &BytesReturned);
    if (!NT_SUCCESS(Status)) {
        goto Exit;
    }

    Irp->IoStatus.Information = BytesReturned;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < BytesReturned) {
        Status = STATUS_BUFFER_OVERFLOW;
        goto Exit;
    }

    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, InfoBuffer.Buffer, BytesReturned);

Exit:

    BounceCleanup(&InfoBuffer);

    return Status;
}

_Use_decl_annotations_
NDIS_STATUS
FilterOidRequest(
    NDIS_HANDLE FilterModuleContext,
    NDIS_OID_REQUEST *Request
    )
{
    LWF_FILTER *Filter = (LWF_FILTER *)FilterModuleContext;
    NDIS_STATUS Status;
    NDIS_OID_REQUEST *ClonedRequest = NULL;
    XDP_OID_CLONE *Context;

    Status =
        NdisAllocateCloneOidRequest(
            Filter->NdisFilterHandle, Request, POOLTAG_OID, &ClonedRequest);
    if (Status != NDIS_STATUS_SUCCESS) {
        goto Exit;
    }

    Context = (XDP_OID_CLONE *)(&ClonedRequest->SourceReserved[0]);
    Context->OriginalRequest = Request;
    ClonedRequest->RequestId = Request->RequestId;

    Status = NdisFOidRequest(Filter->NdisFilterHandle, ClonedRequest);
    if (Status != NDIS_STATUS_PENDING) {
        FilterOidRequestComplete(Filter, ClonedRequest, Status);
        Status = NDIS_STATUS_PENDING;
    }

Exit:

    return Status;
}

_Use_decl_annotations_
VOID
FilterOidRequestComplete(
    NDIS_HANDLE FilterModuleContext,
    NDIS_OID_REQUEST *Request,
    NDIS_STATUS Status
    )
{
    LWF_FILTER *Filter = (LWF_FILTER *)FilterModuleContext;
    NDIS_OID_REQUEST *OriginalRequest;
    XDP_OID_CLONE *Context;

    Context = (XDP_OID_CLONE *)(&Request->SourceReserved[0]);
    OriginalRequest = Context->OriginalRequest;

    if (OriginalRequest == NULL) {
        OidInternalRequestComplete(Filter, Request, Status);
        return;
    }

    //
    // Copy the information from the returned request to the original request
    //
    switch(Request->RequestType)
    {
    case NdisRequestMethod:
        OriginalRequest->DATA.METHOD_INFORMATION.OutputBufferLength =
            Request->DATA.METHOD_INFORMATION.OutputBufferLength;
        OriginalRequest->DATA.METHOD_INFORMATION.BytesRead =
            Request->DATA.METHOD_INFORMATION.BytesRead;
        OriginalRequest->DATA.METHOD_INFORMATION.BytesNeeded =
            Request->DATA.METHOD_INFORMATION.BytesNeeded;
        OriginalRequest->DATA.METHOD_INFORMATION.BytesWritten =
            Request->DATA.METHOD_INFORMATION.BytesWritten;
        break;

    case NdisRequestSetInformation:
        OriginalRequest->DATA.SET_INFORMATION.BytesRead =
            Request->DATA.SET_INFORMATION.BytesRead;
        OriginalRequest->DATA.SET_INFORMATION.BytesNeeded =
            Request->DATA.SET_INFORMATION.BytesNeeded;
        break;

    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
    default:
        OriginalRequest->DATA.QUERY_INFORMATION.BytesWritten =
            Request->DATA.QUERY_INFORMATION.BytesWritten;
        OriginalRequest->DATA.QUERY_INFORMATION.BytesNeeded =
            Request->DATA.QUERY_INFORMATION.BytesNeeded;
        break;
    }

    NdisFreeCloneOidRequest(Filter->NdisFilterHandle, Request);
    NdisFOidRequestComplete(Filter->NdisFilterHandle, OriginalRequest, Status);
}
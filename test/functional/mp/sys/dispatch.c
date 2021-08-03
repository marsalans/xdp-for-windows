//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#include "precomp.h"

static DRIVER_DISPATCH_PAGED MpIoctlDispatch;

static DEVICE_OBJECT *IoctlDeviceObject = NULL;
static NDIS_HANDLE IoctlNdisDeviceObject = NULL;
static INT64 IoctlDeviceReferenceCount = 0;
static EX_PUSH_LOCK IoctlDeviceLock;

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
MpIrpCreate(
    _Inout_ IRP *Irp,
    _In_ IO_STACK_LOCATION *IrpSp
    )
{
    NTSTATUS Status;
    FILE_FULL_EA_INFORMATION *EaBuffer;
    XDPFNMP_OPEN_PACKET *OpenPacket = NULL;
    UCHAR Disposition = 0;
    STRING ExpectedEaName;
    STRING ActualEaName = {0};
    FILE_CREATE_ROUTINE *CreateRoutine = NULL;

#ifdef _WIN64
    if (IoIs32bitProcess(Irp)) {
        Status = STATUS_NOT_SUPPORTED;
        goto Exit;
    }
#endif

    EaBuffer = Irp->AssociatedIrp.SystemBuffer;

    if (EaBuffer == NULL) {
        return STATUS_SUCCESS;
    }

    if (EaBuffer->NextEntryOffset != 0) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    Disposition = (UCHAR)(IrpSp->Parameters.Create.Options >> 24);

    ActualEaName.MaximumLength = EaBuffer->EaNameLength;
    ActualEaName.Length = EaBuffer->EaNameLength;
    ActualEaName.Buffer = EaBuffer->EaName;

    RtlInitString(&ExpectedEaName, XDPFNMP_OPEN_PACKET_NAME);
    if (!RtlEqualString(&ExpectedEaName, &ActualEaName, FALSE)) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    if (EaBuffer->EaValueLength < sizeof(XDPFNMP_OPEN_PACKET)) {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }
    OpenPacket = (XDPFNMP_OPEN_PACKET *)(EaBuffer->EaName + EaBuffer->EaNameLength + 1);

    switch (OpenPacket->ObjectType) {
    case XDPFNMP_FILE_TYPE_GENERIC:
        CreateRoutine = GenericIrpCreate;
        break;

    case XDPFNMP_FILE_TYPE_NATIVE:
        CreateRoutine = NativeIrpCreate;
        break;

    default:
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    Status =
        CreateRoutine(
            Irp, IrpSp, Disposition, OpenPacket + 1,
            EaBuffer->EaValueLength - sizeof(XDPFNMP_OPEN_PACKET));

    if (NT_SUCCESS(Status)) {
        ASSERT(IrpSp->FileObject->FsContext != NULL);
        ASSERT(((FILE_OBJECT_HEADER *)IrpSp->FileObject->FsContext)->Dispatch != NULL);
    }

Exit:

    ASSERT(Status != STATUS_PENDING);

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
MpIrpCleanup(
    _Inout_ IRP *Irp,
    _In_ IO_STACK_LOCATION *IrpSp
    )
{
    FILE_OBJECT_HEADER *FileHeader = IrpSp->FileObject->FsContext;

    if (FileHeader != NULL && FileHeader->Dispatch->Cleanup != NULL) {
        return FileHeader->Dispatch->Cleanup(Irp, IrpSp);
    }

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
MpIrpClose(
    _Inout_ IRP *Irp,
    _In_ IO_STACK_LOCATION *IrpSp
    )
{
    FILE_OBJECT_HEADER *FileHeader = IrpSp->FileObject->FsContext;

    if (FileHeader != NULL && FileHeader->Dispatch->Close != NULL) {
        return FileHeader->Dispatch->Close(Irp, IrpSp);
    }

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
MpIrpIoctl(
    _Inout_ IRP *Irp,
    _In_ IO_STACK_LOCATION *IrpSp
    )
{
    FILE_OBJECT_HEADER *FileHeader;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    Irp->IoStatus.Information = 0;
    FileHeader = IrpSp->FileObject->FsContext;

    if (FileHeader != NULL && FileHeader->Dispatch->IoControl != NULL) {
        Status = FileHeader->Dispatch->IoControl(Irp, IrpSp);
    }

    return Status;
}

static
_Use_decl_annotations_
NTSTATUS
MpIoctlDispatch(
    DEVICE_OBJECT *DeviceObject,
    IRP *Irp
    )
{
    IO_STACK_LOCATION *IrpSp;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);
    PAGED_CODE();

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->MajorFunction) {
    case IRP_MJ_CREATE:
        Status = MpIrpCreate(Irp, IrpSp);
        break;

    case IRP_MJ_CLEANUP:
        Status = MpIrpCleanup(Irp, IrpSp);
        break;

    case IRP_MJ_CLOSE:
        Status = MpIrpClose(Irp, IrpSp);
        break;

    case IRP_MJ_DEVICE_CONTROL:
        Status = MpIrpIoctl(Irp, IrpSp);
        break;

    default:
        break;
    }

    if (Status != STATUS_PENDING) {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return Status;
}

NDIS_STATUS
MpIoctlReference(
    VOID
    )
{
    NDIS_STATUS NdisStatus = NDIS_STATUS_SUCCESS;
    UNICODE_STRING DeviceName;
    UNICODE_STRING DeviceLinkName;
    DRIVER_DISPATCH *DispatchTable[IRP_MJ_MAXIMUM_FUNCTION + 1] = {0};
    NDIS_DEVICE_OBJECT_ATTRIBUTES DeviceAttributes = {0};

    //
    // Lazily create the XDPFNMP device object: NDIS/PNP will not unload the
    // miniport driver if a device object has been created. Therefore, create
    // the device only if non-zero miniports are active.
    //

    ExAcquirePushLockExclusive(&IoctlDeviceLock);

    if (IoctlDeviceReferenceCount == 0) {
        NdisInitUnicodeString(&DeviceName, XDPFNMP_DEVICE_NAME);
        NdisInitUnicodeString(&DeviceLinkName, L"\\DosDevices\\xdpfnmp");

        DeviceAttributes.Header.Type = NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES;
        DeviceAttributes.Header.Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
        DeviceAttributes.Header.Size = sizeof(NDIS_DEVICE_OBJECT_ATTRIBUTES);

        DeviceAttributes.DeviceName = &DeviceName;
        DeviceAttributes.SymbolicName = &DeviceLinkName;
        DeviceAttributes.MajorFunctions = &DispatchTable[0];

        DispatchTable[IRP_MJ_CREATE]            = MpIoctlDispatch;
        DispatchTable[IRP_MJ_CLEANUP]           = MpIoctlDispatch;
        DispatchTable[IRP_MJ_CLOSE]             = MpIoctlDispatch;
        DispatchTable[IRP_MJ_DEVICE_CONTROL]    = MpIoctlDispatch;

        NdisStatus =
            NdisRegisterDeviceEx(
                MpGlobalContext.NdisMiniportDriverHandle, &DeviceAttributes,
                &IoctlDeviceObject, &IoctlNdisDeviceObject);
    }

    if (NdisStatus == NDIS_STATUS_SUCCESS) {
        ASSERT(IoctlDeviceObject != NULL);
        IoctlDeviceReferenceCount++;
    }

    ExReleasePushLockExclusive(&IoctlDeviceLock);

    return NdisStatus;
}

VOID
MpIoctlDereference(
    VOID
    )
{
    ExAcquirePushLockExclusive(&IoctlDeviceLock);

    if (--IoctlDeviceReferenceCount == 0) {
        ASSERT(IoctlDeviceObject != NULL);
        NdisDeregisterDeviceEx(IoctlNdisDeviceObject);
        IoctlNdisDeviceObject = NULL;
        IoctlDeviceObject = NULL;
    }

    ExReleasePushLockExclusive(&IoctlDeviceLock);
}

NDIS_STATUS
MpIoctlStart(
    VOID
    )
{
    ExInitializePushLock(&IoctlDeviceLock);

    return NDIS_STATUS_SUCCESS;
}

VOID
MpIoctlCleanup(
    VOID
    )
{
    ASSERT(IoctlDeviceObject == NULL);
}
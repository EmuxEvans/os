/*++

Copyright (c) 2014 Evan Green

Module Name:

    sdbm2709.c

Abstract:

    This module implements the SD/MMC driver for BCM2709 SoCs.

Author:

    Chris Stevens 10-Dec-2014

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/driver.h>
#include <minoca/intrface/disk.h>
#include <minoca/sd.h>
#include "emmc.h"

//
// ---------------------------------------------------------------- Definitions
//

//
// Define the set of slot flags.
//

#define SD_BCM2709_SLOT_FLAG_INSERTION_PENDING 0x00000001
#define SD_BCM2709_SLOT_FLAG_REMOVAL_PENDING   0x00000002

//
// ------------------------------------------------------ Data Type Definitions
//

typedef enum _SD_BCM2385_DEVICE_TYPE {
    SdBcm2709DeviceInvalid,
    SdBcm2709DeviceBus,
    SdBcm2709DeviceSlot,
    SdBcm2709DeviceDisk
} SD_BCM2709_DEVICE_TYPE, *PSD_BCM2709_DEVICE_TYPE;

typedef struct _SD_BCM2709_BUS SD_BCM2709_BUS, *PSD_BCM2709_BUS;
typedef struct _SD_BCM2709_SLOT SD_BCM2709_SLOT, *PSD_BCM2709_SLOT;

/*++

Structure Description:

    This structure describes an SD/MMC disk context (the context used by the
    bus driver for the disk device).

Members:

    Type - Stores the type identifying this as an SD disk structure.

    ReferenceCount - Stores a reference count for the disk.

    Device - Stores a pointer to the OS device for the disk.

    Parent - Stores a pointer to the parent slot.

    Controller - Stores a pointer to the SD controller structure.

    ControllerLock - Stores a pointer to a lock used to serialize access to the
        controller.

    MediaPresent - Stores a boolean indicating if the disk is still present.

    BlockShift - Stores the block size shift of the disk.

    BlockCount - Stores the number of blocks on the disk.

    DiskInterface - Stores the disk interface presented to the system.

--*/

typedef struct _SD_BCM2709_DISK {
    SD_BCM2709_DEVICE_TYPE Type;
    volatile ULONG ReferenceCount;
    PDEVICE Device;
    PSD_BCM2709_SLOT Parent;
    PSD_CONTROLLER Controller;
    PQUEUED_LOCK ControllerLock;
    BOOL MediaPresent;
    ULONG BlockShift;
    ULONGLONG BlockCount;
    DISK_INTERFACE DiskInterface;
} SD_BCM2709_DISK, *PSD_BCM2709_DISK;

/*++

Structure Description:

    This structure describes an SD/MMC slot (the context used by the bus driver
    for the individual SD slot).

Members:

    Type - Stores the type identifying this as an SD slot.

    Device - Stores a pointer to the OS device for the slot.

    Controller - Stores a pointer to the SD controller structure.

    ControllerBase - Stores the virtual address of the base of the controller
        registers.

    Resource - Stores a pointer to the resource describing the location of the
        controller.

    Parent - Stores a pointer back to the parent.

    Disk - Stores a pointer to the child disk context.

    Lock - Stores a pointer to a lock used to serialize access to the
        controller.

    Flags - Stores a bitmask of slot flags. See SD_BCM2709_SLOT_FLAG_* for
        definitions.

--*/

struct _SD_BCM2709_SLOT {
    SD_BCM2709_DEVICE_TYPE Type;
    PDEVICE Device;
    PSD_CONTROLLER Controller;
    PVOID ControllerBase;
    PRESOURCE_ALLOCATION Resource;
    PSD_BCM2709_BUS Parent;
    PSD_BCM2709_DISK Disk;
    PQUEUED_LOCK Lock;
    volatile ULONG Flags;
};

/*++

Structure Description:

    This structure describes an SD/MMC driver context (the function driver
    context for the SD bus controller).

Members:

    Type - Stores the type identifying this as an SD controller.

    Slots - Stores the array of SD slots.

    Handle - Stores the connected interrupt handle.

    InterruptLine - Stores ths interrupt line of the controller.

    InterruptVector - Stores the interrupt vector of the controller.

    InterruptResourcesFound - Stores a boolean indicating whether or not
        interrupt resources were located for this device.

--*/

struct _SD_BCM2709_BUS {
    SD_BCM2709_DEVICE_TYPE Type;
    SD_BCM2709_SLOT Slot;
    HANDLE InterruptHandle;
    ULONGLONG InterruptLine;
    ULONGLONG InterruptVector;
    BOOL InterruptResourcesFound;
};

//
// ----------------------------------------------- Internal Function Prototypes
//

KSTATUS
SdBcm2709AddDevice (
    PVOID Driver,
    PSTR DeviceId,
    PSTR ClassId,
    PSTR CompatibleIds,
    PVOID DeviceToken
    );

VOID
SdBcm2709DispatchStateChange (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    );

VOID
SdBcm2709DispatchOpen (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    );

VOID
SdBcm2709DispatchClose (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    );

VOID
SdBcm2709DispatchIo (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    );

VOID
SdBcm2709DispatchSystemControl (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    );

INTERRUPT_STATUS
SdBcm2709BusInterruptService (
    PVOID Context
    );

INTERRUPT_STATUS
SdBcm2709BusInterruptServiceDispatch (
    PVOID Context
    );

VOID
SdBcm2709pBusDispatchStateChange (
    PIRP Irp,
    PSD_BCM2709_BUS Bus
    );

VOID
SdBcm2709pSlotDispatchStateChange (
    PIRP Irp,
    PSD_BCM2709_SLOT Slot
    );

VOID
SdBcm2709pDiskDispatchStateChange (
    PIRP Irp,
    PSD_BCM2709_DISK Disk
    );

KSTATUS
SdBcm2709pBusProcessResourceRequirements (
    PIRP Irp,
    PSD_BCM2709_BUS Bus
    );

KSTATUS
SdBcm2709pBusStartDevice (
    PIRP Irp,
    PSD_BCM2709_BUS Bus
    );

KSTATUS
SdBcm2709pBusQueryChildren (
    PIRP Irp,
    PSD_BCM2709_BUS Context
    );

KSTATUS
SdBcm2709pSlotStartDevice (
    PIRP Irp,
    PSD_BCM2709_SLOT Slot
    );

KSTATUS
SdBcm2709pSlotQueryChildren (
    PIRP Irp,
    PSD_BCM2709_SLOT Slot
    );

PSD_BCM2709_DISK
SdBcm2709pCreateDisk (
    PSD_BCM2709_SLOT Slot
    );

VOID
SdBcm2709pDestroyDisk (
    PSD_BCM2709_DISK Disk
    );

VOID
SdBcm2709pDiskAddReference (
    PSD_BCM2709_DISK Disk
    );

VOID
SdBcm2709pDiskReleaseReference (
    PSD_BCM2709_DISK Disk
    );

KSTATUS
SdBcm2709pDiskBlockIoReset (
    PVOID DiskToken
    );

KSTATUS
SdBcm2709pDiskBlockIoRead (
    PVOID DiskToken,
    PIO_BUFFER IoBuffer,
    ULONGLONG BlockAddress,
    UINTN BlockCount,
    PUINTN BlocksCompleted
    );

KSTATUS
SdBcm2709pDiskBlockIoWrite (
    PVOID DiskToken,
    PIO_BUFFER IoBuffer,
    ULONGLONG BlockAddress,
    UINTN BlockCount,
    PUINTN BlocksCompleted
    );

KSTATUS
SdBcm2709pPerformBlockIoPolled (
    PSD_BCM2709_DISK Disk,
    PIO_BUFFER IoBuffer,
    ULONGLONG BlockAddress,
    UINTN BlocksToComplete,
    PUINTN BlocksCompleted,
    BOOL Write,
    BOOL LockRequired
    );

//
// -------------------------------------------------------------------- Globals
//

PDRIVER SdBcm2709Driver = NULL;
UUID SdBcm2709DiskInterfaceUuid = UUID_DISK_INTERFACE;

DISK_INTERFACE SdBcm2709DiskInterfaceTemplate = {
    DISK_INTERFACE_VERSION,
    NULL,
    0,
    0,
    NULL,
    SdBcm2709pDiskBlockIoReset,
    SdBcm2709pDiskBlockIoRead,
    SdBcm2709pDiskBlockIoWrite
};

//
// ------------------------------------------------------------------ Functions
//

KSTATUS
DriverEntry (
    PDRIVER Driver
    )

/*++

Routine Description:

    This routine is the entry point for the SD/MMC driver. It registers its
    other dispatch functions, and performs driver-wide initialization.

Arguments:

    Driver - Supplies a pointer to the driver object.

Return Value:

    STATUS_SUCCESS on success.

    Failure code on error.

--*/

{

    DRIVER_FUNCTION_TABLE FunctionTable;
    KSTATUS Status;

    SdBcm2709Driver = Driver;
    RtlZeroMemory(&FunctionTable, sizeof(DRIVER_FUNCTION_TABLE));
    FunctionTable.Version = DRIVER_FUNCTION_TABLE_VERSION;
    FunctionTable.AddDevice = SdBcm2709AddDevice;
    FunctionTable.DispatchStateChange = SdBcm2709DispatchStateChange;
    FunctionTable.DispatchOpen = SdBcm2709DispatchOpen;
    FunctionTable.DispatchClose = SdBcm2709DispatchClose;
    FunctionTable.DispatchIo = SdBcm2709DispatchIo;
    FunctionTable.DispatchSystemControl = SdBcm2709DispatchSystemControl;
    Status = IoRegisterDriverFunctions(Driver, &FunctionTable);
    return Status;
}

KSTATUS
SdBcm2709AddDevice (
    PVOID Driver,
    PSTR DeviceId,
    PSTR ClassId,
    PSTR CompatibleIds,
    PVOID DeviceToken
    )

/*++

Routine Description:

    This routine is called when a device is detected for which the SD/MMC driver
    acts as the function driver. The driver will attach itself to the stack.

Arguments:

    Driver - Supplies a pointer to the driver being called.

    DeviceId - Supplies a pointer to a string with the device ID.

    ClassId - Supplies a pointer to a string containing the device's class ID.

    CompatibleIds - Supplies a pointer to a string containing device IDs
        that would be compatible with this device.

    DeviceToken - Supplies an opaque token that the driver can use to identify
        the device in the system. This token should be used when attaching to
        the stack.

Return Value:

    STATUS_SUCCESS on success.

    Failure code if the driver was unsuccessful in attaching itself.

--*/

{

    PSD_BCM2709_BUS Context;
    PSD_BCM2709_SLOT Slot;
    KSTATUS Status;

    Context = MmAllocateNonPagedPool(sizeof(SD_BCM2709_BUS), SD_ALLOCATION_TAG);
    if (Context == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Context, sizeof(SD_BCM2709_BUS));
    Context->Type = SdBcm2709DeviceBus;
    Context->InterruptHandle = INVALID_HANDLE;
    Slot = &(Context->Slot);
    Slot->Type = SdBcm2709DeviceSlot;
    Slot->Parent = Context;
    Slot->Flags = SD_BCM2709_SLOT_FLAG_INSERTION_PENDING;
    Status = IoAttachDriverToDevice(Driver, DeviceToken, Context);
    if (!KSUCCESS(Status)) {
        MmFreeNonPagedPool(Context);
    }

    return Status;
}

VOID
SdBcm2709DispatchStateChange (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    )

/*++

Routine Description:

    This routine handles State Change IRPs.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    DeviceContext - Supplies the context pointer supplied by the driver when it
        attached itself to the driver stack. Presumably this pointer contains
        driver-specific device context.

    IrpContext - Supplies the context pointer supplied by the driver when
        the IRP was created.

Return Value:

    None.

--*/

{

    PSD_BCM2709_BUS Context;

    ASSERT(Irp->MajorCode == IrpMajorStateChange);

    Context = (PSD_BCM2709_BUS)DeviceContext;
    switch (Context->Type) {
    case SdBcm2709DeviceBus:
        SdBcm2709pBusDispatchStateChange(Irp, Context);
        break;

    case SdBcm2709DeviceSlot:
        SdBcm2709pSlotDispatchStateChange(Irp, (PSD_BCM2709_SLOT)Context);
        break;

    case SdBcm2709DeviceDisk:
        SdBcm2709pDiskDispatchStateChange(Irp, (PSD_BCM2709_DISK)Context);
        break;

    default:

        ASSERT(FALSE);

        break;
    }

    return;
}

VOID
SdBcm2709DispatchOpen (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    )

/*++

Routine Description:

    This routine handles Open IRPs.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    DeviceContext - Supplies the context pointer supplied by the driver when it
        attached itself to the driver stack. Presumably this pointer contains
        driver-specific device context.

    IrpContext - Supplies the context pointer supplied by the driver when
        the IRP was created.

Return Value:

    None.

--*/

{

    PSD_BCM2709_DISK Disk;

    Disk = DeviceContext;
    if (Disk->Type != SdBcm2709DeviceDisk) {
        return;
    }

    SdBcm2709pDiskAddReference(Disk);
    IoCompleteIrp(SdBcm2709Driver, Irp, STATUS_SUCCESS);
    return;
}

VOID
SdBcm2709DispatchClose (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    )

/*++

Routine Description:

    This routine handles Close IRPs.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    DeviceContext - Supplies the context pointer supplied by the driver when it
        attached itself to the driver stack. Presumably this pointer contains
        driver-specific device context.

    IrpContext - Supplies the context pointer supplied by the driver when
        the IRP was created.

Return Value:

    None.

--*/

{

    PSD_BCM2709_DISK Disk;

    Disk = DeviceContext;
    if (Disk->Type != SdBcm2709DeviceDisk) {
        return;
    }

    SdBcm2709pDiskReleaseReference(Disk);
    IoCompleteIrp(SdBcm2709Driver, Irp, STATUS_SUCCESS);
    return;
}

VOID
SdBcm2709DispatchIo (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    )

/*++

Routine Description:

    This routine handles I/O IRPs.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    DeviceContext - Supplies the context pointer supplied by the driver when it
        attached itself to the driver stack. Presumably this pointer contains
        driver-specific device context.

    IrpContext - Supplies the context pointer supplied by the driver when
        the IRP was created.

Return Value:

    None.

--*/

{

    UINTN BlockCount;
    ULONGLONG BlockOffset;
    UINTN BlocksCompleted;
    UINTN BytesCompleted;
    UINTN BytesToComplete;
    PSD_BCM2709_DISK Disk;
    PIO_BUFFER IoBuffer;
    ULONGLONG IoOffset;
    KSTATUS Status;
    BOOL Write;

    ASSERT(KeGetRunLevel() == RunLevelLow);
    ASSERT(Irp->Direction == IrpDown);

    Disk = DeviceContext;
    if (Disk->Type != SdBcm2709DeviceDisk) {

        ASSERT(FALSE);

        return;
    }

    if (Disk->MediaPresent == FALSE) {
        Status = STATUS_NO_MEDIA;
        goto DispatchIoEnd;
    }

    Write = FALSE;
    if (Irp->MinorCode == IrpMinorIoWrite) {
        Write = TRUE;
    }

    BytesToComplete = Irp->U.ReadWrite.IoSizeInBytes;
    IoOffset = Irp->U.ReadWrite.IoOffset;
    IoBuffer = Irp->U.ReadWrite.IoBuffer;

    ASSERT((Disk->BlockCount != 0) && (Disk->BlockShift != 0));
    ASSERT(IoBuffer != NULL);
    ASSERT(IS_ALIGNED(IoOffset, 1 << Disk->BlockShift) != FALSE);
    ASSERT(IS_ALIGNED(BytesToComplete, 1 << Disk->BlockShift) != FALSE);

    BlockOffset = IoOffset >> Disk->BlockShift;
    BlockCount = BytesToComplete >> Disk->BlockShift;
    Status = SdBcm2709pPerformBlockIoPolled(Disk,
                                            IoBuffer,
                                            BlockOffset,
                                            BlockCount,
                                            &BlocksCompleted,
                                            Write,
                                            TRUE);

    BytesCompleted = BlocksCompleted << Disk->BlockShift;
    Irp->U.ReadWrite.IoBytesCompleted = BytesCompleted;
    Irp->U.ReadWrite.NewIoOffset = IoOffset + BytesCompleted;

DispatchIoEnd:
    IoCompleteIrp(SdBcm2709Driver, Irp, Status);
    return;
}

VOID
SdBcm2709DispatchSystemControl (
    PIRP Irp,
    PVOID DeviceContext,
    PVOID IrpContext
    )

/*++

Routine Description:

    This routine handles System Control IRPs.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    DeviceContext - Supplies the context pointer supplied by the driver when it
        attached itself to the driver stack. Presumably this pointer contains
        driver-specific device context.

    IrpContext - Supplies the context pointer supplied by the driver when
        the IRP was created.

Return Value:

    None.

--*/

{

    PVOID Context;
    PSD_BCM2709_DISK Disk;
    PSYSTEM_CONTROL_FILE_OPERATION FileOperation;
    PSYSTEM_CONTROL_LOOKUP Lookup;
    PFILE_PROPERTIES Properties;
    ULONGLONG PropertiesFileSize;
    KSTATUS Status;

    Context = Irp->U.SystemControl.SystemContext;
    Disk = DeviceContext;

    //
    // Only disk devices are supported.
    //

    if (Disk->Type != SdBcm2709DeviceDisk) {
        return;
    }

    switch (Irp->MinorCode) {
    case IrpMinorSystemControlLookup:
        Lookup = (PSYSTEM_CONTROL_LOOKUP)Context;
        Status = STATUS_PATH_NOT_FOUND;
        if (Lookup->Root != FALSE) {

            //
            // Enable opening of the root as a single file.
            //

            Properties = &(Lookup->Properties);
            Properties->FileId = 0;
            Properties->Type = IoObjectBlockDevice;
            Properties->HardLinkCount = 1;
            Properties->BlockCount = Disk->BlockCount;
            Properties->BlockSize = 1 << Disk->BlockShift;
            WRITE_INT64_SYNC(&(Properties->FileSize),
                             Disk->BlockCount << Disk->BlockShift);

            Status = STATUS_SUCCESS;
        }

        IoCompleteIrp(SdBcm2709Driver, Irp, Status);
        break;

    //
    // Writes to the disk's properties are not allowed. Fail if the data
    // has changed.
    //

    case IrpMinorSystemControlWriteFileProperties:
        FileOperation = (PSYSTEM_CONTROL_FILE_OPERATION)Context;
        Properties = FileOperation->FileProperties;
        READ_INT64_SYNC(&(Properties->FileSize), &PropertiesFileSize);
        if ((Properties->FileId != 0) ||
            (Properties->Type != IoObjectBlockDevice) ||
            (Properties->HardLinkCount != 1) ||
            (Properties->BlockSize != (1 << Disk->BlockShift)) ||
            (Properties->BlockCount != Disk->BlockCount) ||
            (PropertiesFileSize != (Disk->BlockCount << Disk->BlockShift))) {

            Status = STATUS_NOT_SUPPORTED;

        } else {
            Status = STATUS_SUCCESS;
        }

        IoCompleteIrp(SdBcm2709Driver, Irp, Status);
        break;

    //
    // Do not support hard disk device truncation.
    //

    case IrpMinorSystemControlTruncate:
        IoCompleteIrp(SdBcm2709Driver, Irp, STATUS_NOT_SUPPORTED);
        break;

    //
    // Gather and return device information.
    //

    case IrpMinorSystemControlDeviceInformation:
        break;

    case IrpMinorSystemControlSynchronize:
        IoCompleteIrp(SdBcm2709Driver, Irp, STATUS_SUCCESS);
        break;

    //
    // Ignore everything unrecognized.
    //

    default:

        ASSERT(FALSE);

        break;
    }

    return;
}

INTERRUPT_STATUS
SdBcm2709BusInterruptService (
    PVOID Context
    )

/*++

Routine Description:

    This routine implements the interrupt service routine for an SD bus.

Arguments:

    Context - Supplies a pointer to the device context.

Return Value:

    Returns whether or not the SD controller caused the interrupt.

--*/

{

    PSD_BCM2709_BUS Bus;
    PSD_BCM2709_SLOT Slot;
    INTERRUPT_STATUS Status;

    Bus = Context;
    Slot = &(Bus->Slot);
    Status = InterruptStatusNotClaimed;
    if (Slot->Controller != NULL) {
        Status = SdStandardInterruptService(Slot->Controller);
    }

    return Status;
}

INTERRUPT_STATUS
SdBcm2709BusInterruptServiceDispatch (
    PVOID Context
    )

/*++

Routine Description:

    This routine implements the dispatch level interrupt service routine for an
    SD bus.

Arguments:

    Context - Supplies a pointer to the device context.

Return Value:

    Returns whether or not the SD controller caused the interrupt.

--*/

{

    PSD_BCM2709_BUS Bus;
    PSD_BCM2709_SLOT Slot;
    INTERRUPT_STATUS Status;

    Bus = Context;
    Slot = &(Bus->Slot);
    Status = InterruptStatusNotClaimed;
    if (Slot->Controller != NULL) {
        Status = SdStandardInterruptServiceDispatch(Slot->Controller);
    }

    return Status;
}

VOID
SdBcm2709pBusDispatchStateChange (
    PIRP Irp,
    PSD_BCM2709_BUS Bus
    )

/*++

Routine Description:

    This routine handles State Change IRPs for the SD bus device.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    Bus - Supplies a pointer to the SD bus.

Return Value:

    None.

--*/

{

    KSTATUS Status;

    if (Irp->Direction == IrpUp) {
        if (!KSUCCESS(IoGetIrpStatus(Irp))) {
            return;
        }

        switch (Irp->MinorCode) {
        case IrpMinorQueryResources:
            Status = SdBcm2709pBusProcessResourceRequirements(Irp, Bus);
            if (!KSUCCESS(Status)) {
                IoCompleteIrp(SdBcm2709Driver, Irp, Status);
            }

            break;

        case IrpMinorStartDevice:
            Status = SdBcm2709pBusStartDevice(Irp, Bus);
            if (!KSUCCESS(Status)) {
                IoCompleteIrp(SdBcm2709Driver, Irp, Status);
            }

            break;

        case IrpMinorQueryChildren:
            Status = SdBcm2709pBusQueryChildren(Irp, Bus);
            if (!KSUCCESS(Status)) {
                IoCompleteIrp(SdBcm2709Driver, Irp, Status);
            }

            break;

        default:
            break;
        }
    }

    return;
}

VOID
SdBcm2709pSlotDispatchStateChange (
    PIRP Irp,
    PSD_BCM2709_SLOT Slot
    )

/*++

Routine Description:

    This routine handles State Change IRPs for the SD bus device.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    Slot - Supplies a pointer to the SD slot.

Return Value:

    None.

--*/

{

    KSTATUS Status;

    //
    // Actively handle IRPs as the bus driver for the slot.
    //

    if (Irp->Direction == IrpDown) {
        switch (Irp->MinorCode) {
        case IrpMinorStartDevice:
            Status = SdBcm2709pSlotStartDevice(Irp, Slot);
            IoCompleteIrp(SdBcm2709Driver, Irp, Status);
            break;

        case IrpMinorQueryResources:
            IoCompleteIrp(SdBcm2709Driver, Irp, STATUS_SUCCESS);
            break;

        case IrpMinorQueryChildren:
            Status = SdBcm2709pSlotQueryChildren(Irp, Slot);
            IoCompleteIrp(SdBcm2709Driver, Irp, Status);
            break;

        default:
            break;
        }
    }

    return;
}

VOID
SdBcm2709pDiskDispatchStateChange (
    PIRP Irp,
    PSD_BCM2709_DISK Disk
    )

/*++

Routine Description:

    This routine handles State Change IRPs for a parent device.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    Disk - Supplies a pointer to the SD disk.

Return Value:

    None.

--*/

{

    BOOL CompleteIrp;
    KSTATUS Status;

    ASSERT(Irp->MajorCode == IrpMajorStateChange);

    //
    // The IRP is on its way down the stack. Do most processing here.
    //

    if (Irp->Direction == IrpDown) {
        Status = STATUS_NOT_SUPPORTED;
        CompleteIrp = TRUE;
        switch (Irp->MinorCode) {
        case IrpMinorQueryResources:
            Status = STATUS_SUCCESS;
            break;

        case IrpMinorStartDevice:

            //
            // Publish the disk interface.
            //

            Status = STATUS_SUCCESS;
            if (Disk->DiskInterface.DiskToken == NULL) {
                RtlCopyMemory(&(Disk->DiskInterface),
                              &SdBcm2709DiskInterfaceTemplate,
                              sizeof(DISK_INTERFACE));

                Disk->DiskInterface.DiskToken = Disk;
                Disk->DiskInterface.BlockSize = 1 << Disk->BlockShift;
                Disk->DiskInterface.BlockCount = Disk->BlockCount;
                Status = IoCreateInterface(&SdBcm2709DiskInterfaceUuid,
                                           Disk->Device,
                                           &(Disk->DiskInterface),
                                           sizeof(DISK_INTERFACE));

                if (!KSUCCESS(Status)) {
                    Disk->DiskInterface.DiskToken = NULL;
                }
            }

            break;

        case IrpMinorQueryChildren:
            Irp->U.QueryChildren.Children = NULL;
            Irp->U.QueryChildren.ChildCount = 0;
            Status = STATUS_SUCCESS;
            break;

        case IrpMinorQueryInterface:
            break;

        case IrpMinorRemoveDevice:
            if (Disk->DiskInterface.DiskToken != NULL) {
                Status = IoDestroyInterface(&SdBcm2709DiskInterfaceUuid,
                                            Disk->Device,
                                            &(Disk->DiskInterface));

                ASSERT(KSUCCESS(Status));

                Disk->DiskInterface.DiskToken = NULL;
            }

            SdBcm2709pDiskReleaseReference(Disk);
            Status = STATUS_SUCCESS;
            break;

        //
        // Pass all other IRPs down.
        //

        default:
            CompleteIrp = FALSE;
            break;
        }

        //
        // Complete the IRP unless there's a reason not to.
        //

        if (CompleteIrp != FALSE) {
            IoCompleteIrp(SdBcm2709Driver, Irp, Status);
        }

    //
    // The IRP is completed and is on its way back up.
    //

    } else {

        ASSERT(Irp->Direction == IrpUp);
    }

    return;
}

KSTATUS
SdBcm2709pBusProcessResourceRequirements (
    PIRP Irp,
    PSD_BCM2709_BUS Bus
    )

/*++

Routine Description:

    This routine filters through the resource requirements presented by the
    bus for a SD Bus controller. It adds an interrupt vector requirement
    for any interrupt line requested.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    Bus - Supplies a pointer to the bus context.

Return Value:

    Status code.

--*/

{

    PRESOURCE_CONFIGURATION_LIST Requirements;
    KSTATUS Status;
    RESOURCE_REQUIREMENT VectorRequirement;

    ASSERT((Irp->MajorCode == IrpMajorStateChange) &&
           (Irp->MinorCode == IrpMinorQueryResources));

    //
    // Initialize a nice interrupt vector requirement in preparation.
    //

    RtlZeroMemory(&VectorRequirement, sizeof(RESOURCE_REQUIREMENT));
    VectorRequirement.Type = ResourceTypeInterruptVector;
    VectorRequirement.Minimum = 0;
    VectorRequirement.Maximum = -1;
    VectorRequirement.Length = 1;

    //
    // Loop through all configuration lists, creating a vector for each line.
    //

    Requirements = Irp->U.QueryResources.ResourceRequirements;
    Status = IoCreateAndAddInterruptVectorsForLines(Requirements,
                                                    &VectorRequirement);

    if (!KSUCCESS(Status)) {
        goto BusProcessResourceRequirementsEnd;
    }

BusProcessResourceRequirementsEnd:
    return Status;
}

KSTATUS
SdBcm2709pBusStartDevice (
    PIRP Irp,
    PSD_BCM2709_BUS Bus
    )

/*++

Routine Description:

    This routine starts an SD bus device.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    Bus - Supplies a pointer to the bus context.

Return Value:

    Status code.

--*/

{

    PRESOURCE_ALLOCATION Allocation;
    PRESOURCE_ALLOCATION_LIST AllocationList;
    IO_CONNECT_INTERRUPT_PARAMETERS Connect;
    PRESOURCE_ALLOCATION LineAllocation;
    KSTATUS Status;

    ASSERT(Bus->Slot.Controller == NULL);
    ASSERT(Bus->Slot.Resource == NULL);

    //
    // Loop through the allocated resources to get the controller base and the
    // interrupt.
    //

    AllocationList = Irp->U.StartDevice.ProcessorLocalResources;
    Allocation = IoGetNextResourceAllocation(AllocationList, NULL);
    while (Allocation != NULL) {

        //
        // If the resource is an interrupt vector, then it should have an
        // owning interrupt line allocation.
        //

        if (Allocation->Type == ResourceTypeInterruptVector) {

            //
            // Currently only one interrupt resource is expected.
            //

            ASSERT(Bus->InterruptResourcesFound == FALSE);
            ASSERT(Allocation->OwningAllocation != NULL);

            //
            // Save the line and vector number.
            //

            LineAllocation = Allocation->OwningAllocation;
            Bus->InterruptLine = LineAllocation->Allocation;
            Bus->InterruptVector = Allocation->Allocation;
            Bus->InterruptResourcesFound = TRUE;

        } else if (Allocation->Type == ResourceTypePhysicalAddressSpace) {
            if ((Bus->Slot.Resource == NULL) && (Allocation->Length > 0)) {
                Bus->Slot.Resource = Allocation;
            }
        }

        //
        // Get the next allocation in the list.
        //

        Allocation = IoGetNextResourceAllocation(AllocationList, Allocation);
    }

    //
    // Attempt to connect the interrupt.
    //

    if (Bus->InterruptHandle == INVALID_HANDLE) {
        RtlZeroMemory(&Connect, sizeof(IO_CONNECT_INTERRUPT_PARAMETERS));
        Connect.Version = IO_CONNECT_INTERRUPT_PARAMETERS_VERSION;
        Connect.Device = Irp->Device;
        Connect.LineNumber = Bus->InterruptLine;
        Connect.Vector = Bus->InterruptVector;
        Connect.InterruptServiceRoutine = SdBcm2709BusInterruptService;
        Connect.DispatchServiceRoutine = SdBcm2709BusInterruptServiceDispatch;
        Connect.Context = Bus;
        Connect.Interrupt = &(Bus->InterruptHandle);
        Status = IoConnectInterrupt(&Connect);
        if (!KSUCCESS(Status)) {
            goto StartDeviceEnd;
        }
    }

    Status = STATUS_SUCCESS;

StartDeviceEnd:
    if (!KSUCCESS(Status)) {
        if (Bus->InterruptHandle != INVALID_HANDLE) {
            IoDisconnectInterrupt(Bus->InterruptHandle);
            Bus->InterruptHandle = INVALID_HANDLE;
        }
    }

    return Status;
}

KSTATUS
SdBcm2709pBusQueryChildren (
    PIRP Irp,
    PSD_BCM2709_BUS Context
    )

/*++

Routine Description:

    This routine handles State Change IRPs for the SD bus device.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    Context - Supplies a pointer to the bus context.

Return Value:

    Status code.

--*/

{

    PSD_BCM2709_SLOT Slot;
    KSTATUS Status;

    Slot = &(Context->Slot);
    if (Slot->Resource == NULL) {
        return STATUS_SUCCESS;
    }

    if (Slot->Device == NULL) {
        Status = IoCreateDevice(SdBcm2709Driver,
                                Slot,
                                Irp->Device,
                                SD_SLOT_DEVICE_ID,
                                NULL,
                                NULL,
                                &(Slot->Device));

        if (!KSUCCESS(Status)) {
            goto BusQueryChildrenEnd;
        }
    }

    ASSERT(Slot->Device != NULL);

    Status = IoMergeChildArrays(Irp,
                                &(Slot->Device),
                                1,
                                SD_ALLOCATION_TAG);

    if (!KSUCCESS(Status)) {
        goto BusQueryChildrenEnd;
    }

BusQueryChildrenEnd:
    return Status;
}

KSTATUS
SdBcm2709pSlotStartDevice (
    PIRP Irp,
    PSD_BCM2709_SLOT Slot
    )

/*++

Routine Description:

    This routine starts an SD slot device.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    Slot - Supplies a pointer to the slot context.

Return Value:

    Status code.

--*/

{

    ULONG Frequency;
    SD_INITIALIZATION_BLOCK Parameters;
    KSTATUS Status;

    ASSERT(Slot->Resource != NULL);

    //
    // Initialize the controller base.
    //

    if (Slot->ControllerBase == NULL) {
        Slot->ControllerBase = MmMapPhysicalAddress(Slot->Resource->Allocation,
                                                    Slot->Resource->Length,
                                                    TRUE,
                                                    FALSE,
                                                    TRUE);

        if (Slot->ControllerBase == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto StartDeviceEnd;
        }
    }

    if (Slot->Lock == NULL) {
        Slot->Lock = KeCreateQueuedLock();
        if (Slot->Lock == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto StartDeviceEnd;
        }
    }

    //
    // Initialize the standard SD controller.
    //

    if (Slot->Controller == NULL) {

        //
        // Power on the BCM2709's Emmc.
        //

        Status = Bcm2709EmmcInitialize();
        if (!KSUCCESS(Status)) {
            goto StartDeviceEnd;
        }

        Status = Bcm2709EmmcGetClockFrequency(&Frequency);
        if (!KSUCCESS(Status)) {
            goto StartDeviceEnd;
        }

        RtlZeroMemory(&Parameters, sizeof(SD_INITIALIZATION_BLOCK));
        Parameters.ConsumerContext = Slot;
        Parameters.StandardControllerBase = Slot->ControllerBase;
        Parameters.Voltages = SD_VOLTAGE_32_33 |
                              SD_VOLTAGE_33_34 |
                              SD_VOLTAGE_165_195;

        Parameters.HostCapabilities = SD_MODE_AUTO_CMD12 |
                                      SD_MODE_4BIT |
                                      SD_MODE_RESPONSE136_SHIFTED |
                                      SD_MODE_HIGH_SPEED |
                                      SD_MODE_HIGH_SPEED_52MHZ;

        Parameters.FundamentalClock = Frequency;
        Slot->Controller = SdCreateController(&Parameters);
        if (Slot->Controller == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto StartDeviceEnd;
        }

        Slot->Controller->InterruptHandle = Slot->Parent->InterruptHandle;
    }

    Status = STATUS_SUCCESS;

StartDeviceEnd:
    if (!KSUCCESS(Status)) {
        if (Slot->Lock != NULL) {
            KeDestroyQueuedLock(Slot->Lock);
        }

        if (Slot->Controller != NULL) {
            SdDestroyController(Slot->Controller);
            Slot->Controller = NULL;
        }
    }

    return Status;
}

KSTATUS
SdBcm2709pSlotQueryChildren (
    PIRP Irp,
    PSD_BCM2709_SLOT Slot
    )

/*++

Routine Description:

    This routine potentially enumerates an SD card in a given slot.

Arguments:

    Irp - Supplies a pointer to the I/O request packet.

    Slot - Supplies a pointer to the SD slot that may contain the card.

Return Value:

    Status code.

--*/

{

    ULONG BlockSize;
    ULONG FlagsMask;
    PSD_BCM2709_DISK NewDisk;
    ULONG OldFlags;
    KSTATUS Status;

    NewDisk = NULL;

    //
    // The Broadcom SD chip does not currently support device insertion and
    // removal, but at least handle it here for the initial query.
    //

    FlagsMask = ~(SD_BCM2709_SLOT_FLAG_INSERTION_PENDING |
                  SD_BCM2709_SLOT_FLAG_REMOVAL_PENDING);

    OldFlags = RtlAtomicAnd32(&(Slot->Flags), FlagsMask);

    //
    // If either insertion or removal is pending, remove the existing disk. In
    // practice, an insertion can occur without the previous removal.
    //

    FlagsMask = SD_BCM2709_SLOT_FLAG_INSERTION_PENDING |
                SD_BCM2709_SLOT_FLAG_REMOVAL_PENDING;

    if ((OldFlags & FlagsMask) != 0) {
        if (Slot->Disk != NULL) {
            KeAcquireQueuedLock(Slot->Lock);
            Slot->Disk->MediaPresent = FALSE;
            KeReleaseQueuedLock(Slot->Lock);
            Slot->Disk = NULL;
        }
    }

    //
    // If an insertion is pending, try to enumerate the new disk.
    //

    if ((OldFlags & SD_BCM2709_SLOT_FLAG_INSERTION_PENDING) != 0) {

        ASSERT(Slot->Disk == NULL);

        //
        // Initialize the controller to see if a disk is actually present.
        //

        Status = SdInitializeController(Slot->Controller, TRUE);
        if (!KSUCCESS(Status)) {
            if (Status == STATUS_TIMEOUT) {
                Status = STATUS_SUCCESS;
            }

            goto SlotQueryChildrenEnd;
        }

        //
        // A disk was found to be present. Create state for it.
        //

        NewDisk = SdBcm2709pCreateDisk(Slot);
        if (NewDisk == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto SlotQueryChildrenEnd;
        }

        BlockSize = 0;
        Status = SdGetMediaParameters(NewDisk->Controller,
                                      &(NewDisk->BlockCount),
                                      &BlockSize);

        if (!KSUCCESS(Status)) {
            if (Status == STATUS_NO_MEDIA) {
                Status = STATUS_SUCCESS;
            }

            goto SlotQueryChildrenEnd;
        }

        ASSERT(POWER_OF_2(BlockSize) != FALSE);

        NewDisk->BlockShift = RtlCountTrailingZeros32(BlockSize);
        NewDisk->MediaPresent = TRUE;

        //
        // Create the child device.
        //

        Status = IoCreateDevice(SdBcm2709Driver,
                                NewDisk,
                                Irp->Device,
                                SD_CARD_DEVICE_ID,
                                DISK_CLASS_ID,
                                NULL,
                                &(NewDisk->Device));

        if (!KSUCCESS(Status)) {
            goto SlotQueryChildrenEnd;
        }

        Slot->Disk = NewDisk;
        NewDisk = NULL;
    }

    //
    // If there's no disk, don't enumerate it.
    //

    if (Slot->Disk == NULL) {
        Status = STATUS_SUCCESS;
        goto SlotQueryChildrenEnd;
    }

    ASSERT((Slot->Disk != NULL) && (Slot->Disk->Device != NULL));

    //
    // Enumerate the one child.
    //

    Status = IoMergeChildArrays(Irp,
                                &(Slot->Disk->Device),
                                1,
                                SD_ALLOCATION_TAG);

    if (!KSUCCESS(Status)) {
        goto SlotQueryChildrenEnd;
    }

SlotQueryChildrenEnd:
    if (NewDisk != NULL) {

        ASSERT(NewDisk->Device == NULL);

        SdBcm2709pDiskReleaseReference(NewDisk);
    }

    return Status;
}

PSD_BCM2709_DISK
SdBcm2709pCreateDisk (
    PSD_BCM2709_SLOT Slot
    )

/*++

Routine Description:

    This routine creates an SD disk context.

Arguments:

    Slot - Supplies a pointer to the SD slot to which the disk belongs.

Return Value:

    Returns a pointer to the new SD disk on success or NULL on failure.

--*/

{

    PSD_BCM2709_DISK Disk;

    Disk = MmAllocateNonPagedPool(sizeof(SD_BCM2709_DISK), SD_ALLOCATION_TAG);
    if (Disk == NULL) {
        return NULL;
    }

    RtlZeroMemory(Disk, sizeof(SD_BCM2709_DISK));
    Disk->Type = SdBcm2709DeviceDisk;
    Disk->Parent = Slot;
    Disk->Controller = Slot->Controller;
    Disk->ControllerLock = Slot->Lock;
    Disk->ReferenceCount = 1;
    return Disk;
}

VOID
SdBcm2709pDestroyDisk (
    PSD_BCM2709_DISK Disk
    )

/*++

Routine Description:

    This routine destroys the given SD disk.

Arguments:

    Disk - Supplies a pointer to the SD disk to destroy.

Return Value:

    None.

--*/

{

    ASSERT((Disk->MediaPresent == FALSE) || (Disk->Device == NULL));
    ASSERT(Disk->DiskInterface.DiskToken == NULL);

    MmFreeNonPagedPool(Disk);
    return;
}

VOID
SdBcm2709pDiskAddReference (
    PSD_BCM2709_DISK Disk
    )

/*++

Routine Description:

    This routine adds a reference to SD disk.

Arguments:

    Disk - Supplies a pointer to the SD disk.

Return Value:

    None.

--*/

{

    ULONG OldReferenceCount;

    OldReferenceCount = RtlAtomicAdd32(&(Disk->ReferenceCount), 1);

    ASSERT((OldReferenceCount != 0) && (OldReferenceCount < 0x10000000));

    return;
}

VOID
SdBcm2709pDiskReleaseReference (
    PSD_BCM2709_DISK Disk
    )

/*++

Routine Description:

    This routine releases a reference from the SD disk.

Arguments:

    Disk - Supplies a pointer to the SD disk.

Return Value:

    None.

--*/

{

    ULONG OldReferenceCount;

    OldReferenceCount = RtlAtomicAdd32(&(Disk->ReferenceCount), (ULONG)-1);

    ASSERT((OldReferenceCount != 0) && (OldReferenceCount < 0x10000000));

    if (OldReferenceCount == 1) {
        SdBcm2709pDestroyDisk(Disk);
    }

    return;
}

KSTATUS
SdBcm2709pDiskBlockIoReset (
    PVOID DiskToken
    )

/*++

Routine Description:

    This routine must be called immediately before using the block read and
    write routines in order to allow the disk to reset any I/O channels in
    preparation for imminent block I/O. This routine is called at high run
    level.

Arguments:

    DiskToken - Supplies an opaque token for the disk. The appropriate token is
        retrieved by querying the disk device information.

Return Value:

    Status code.

--*/

{

    PSD_BCM2709_DISK Disk;
    KSTATUS Status;

    ASSERT(KeGetRunLevel() == RunLevelHigh);

    Disk = (PSD_BCM2709_DISK)DiskToken;

    //
    // Put the SD controller into critical execution mode.
    //

    SdSetCriticalMode(Disk->Controller, TRUE);

    //
    // Abort any current transaction that might have been left incomplete
    // when the crash occurred.
    //

    Status = SdAbortTransaction(Disk->Controller, FALSE);
    return Status;
}

KSTATUS
SdBcm2709pDiskBlockIoRead (
    PVOID DiskToken,
    PIO_BUFFER IoBuffer,
    ULONGLONG BlockAddress,
    UINTN BlockCount,
    PUINTN BlocksCompleted
    )

/*++

Routine Description:

    This routine reads the block contents from the disk into the given I/O
    buffer using polled I/O. It does so without acquiring any locks or
    allocating any resources, as this routine is used for crash dump support
    when the system is in a very fragile state. This routine must be called at
    high level.

Arguments:

    DiskToken - Supplies an opaque token for the disk. The appropriate token is
        retrieved by querying the disk device information.

    IoBuffer - Supplies a pointer to the I/O buffer where the data will be read.

    BlockAddress - Supplies the block index to read (for physical disk, this is
        the LBA).

    BlockCount - Supplies the number of blocks to read.

    BlocksCompleted - Supplies a pointer that receives the total number of
        blocks read.

Return Value:

    Status code.

--*/

{

    KSTATUS Status;

    ASSERT(KeGetRunLevel() == RunLevelHigh);

    //
    // As this read routine is meant for critical code paths (crash dump),
    // indicate that the channel should not be locked when performing the I/O.
    // It may be that some other thread holds the lock, which would cause a
    // dead lock as all other processors and threads are likely frozen.
    //

    Status = SdBcm2709pPerformBlockIoPolled(DiskToken,
                                            IoBuffer,
                                            BlockAddress,
                                            BlockCount,
                                            BlocksCompleted,
                                            FALSE,
                                            FALSE);

    return Status;
}

KSTATUS
SdBcm2709pDiskBlockIoWrite (
    PVOID DiskToken,
    PIO_BUFFER IoBuffer,
    ULONGLONG BlockAddress,
    UINTN BlockCount,
    PUINTN BlocksCompleted
    )

/*++

Routine Description:

    This routine writes the contents of the given I/O buffer to the disk using
    polled I/O. It does so without acquiring any locks or allocating any
    resources, as this routine is used for crash dump support when the system
    is in a very fragile state. This routine must be called at high level.

Arguments:

    DiskToken - Supplies an opaque token for the disk. The appropriate token is
        retrieved by querying the disk device information.

    IoBuffer - Supplies a pointer to the I/O buffer containing the data to
        write.

    BlockAddress - Supplies the block index to write to (for physical disk,
        this is the LBA).

    BlockCount - Supplies the number of blocks to write.

    BlocksCompleted - Supplies a pointer that receives the total number of
        blocks written.

Return Value:

    Status code.

--*/

{

    KSTATUS Status;

    ASSERT(KeGetRunLevel() == RunLevelHigh);

    //
    // As this write routine is meant for critical code paths (crash dump),
    // indicate that the channel should not be locked when performing the I/O.
    // It may be that some other thread holds the lock, which would cause a
    // dead lock as all other processors and threads are likely frozen.
    //

    Status = SdBcm2709pPerformBlockIoPolled(DiskToken,
                                            IoBuffer,
                                            BlockAddress,
                                            BlockCount,
                                            BlocksCompleted,
                                            TRUE,
                                            FALSE);

    return Status;
}

KSTATUS
SdBcm2709pPerformBlockIoPolled (
    PSD_BCM2709_DISK Disk,
    PIO_BUFFER IoBuffer,
    ULONGLONG BlockAddress,
    UINTN BlocksToComplete,
    PUINTN BlocksCompleted,
    BOOL Write,
    BOOL LockRequired
    )

/*++

Routine Description:

    This routine performs polled I/O data transfers.

Arguments:

    Disk - Supplies a pointer to the SD disk device.

    IoBuffer - Supplies a pointer to the I/O buffer to use for read or write.

    BlockAddress - Supplies the block number to read from or write to (LBA).

    BlocksToComplete - Supplies the number of blocks to read or write.

    BlocksCompleted - Supplies a pointer that receives the total number of
        blocks read or written.

    Write - Supplies a boolean indicating if this is a read operation (TRUE) or
        a write operation (FALSE).

    LockRequired - Supplies a boolean indicating if the controller lock needs
        to be acquired (TRUE) or it does not (FALSE).

Return Value:

    None.

--*/

{

    UINTN BlockCount;
    ULONGLONG BlockOffset;
    UINTN BlocksComplete;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    UINTN FragmentOffset;
    UINTN FragmentSize;
    UINTN IoBufferOffset;
    BOOL LockHeld;
    PIO_BUFFER OriginalIoBuffer;
    PHYSICAL_ADDRESS PhysicalAddress;
    KSTATUS Status;
    PVOID VirtualAddress;

    BlocksComplete = 0;
    LockHeld = FALSE;

    ASSERT(IoBuffer != NULL);
    ASSERT((Disk->BlockCount != 0) && (Disk->BlockShift != 0));

    //
    // Validate the supplied I/O buffer is aligned and big enough.
    //

    OriginalIoBuffer = IoBuffer;
    Status = MmValidateIoBuffer(0,
                                MAX_ULONGLONG,
                                1 << Disk->BlockShift,
                                BlocksToComplete << Disk->BlockShift,
                                FALSE,
                                &IoBuffer);

    if (!KSUCCESS(Status)) {
        goto PerformBlockIoPolledEnd;
    }

    if ((IoBuffer != OriginalIoBuffer) && (Write != FALSE)) {
        Status = MmCopyIoBuffer(IoBuffer,
                                0,
                                OriginalIoBuffer,
                                0,
                                BlocksToComplete << Disk->BlockShift);

        if (!KSUCCESS(Status)) {
            goto PerformBlockIoPolledEnd;
        }
    }

    //
    // Make sure the I/O buffer is mapped before use. SD depends on the buffer
    // being mapped.
    //

    Status = MmMapIoBuffer(IoBuffer, FALSE, FALSE, FALSE);
    if (!KSUCCESS(Status)) {
        goto PerformBlockIoPolledEnd;
    }

    //
    // Find the starting fragment based on the current offset.
    //

    IoBufferOffset = MmGetIoBufferCurrentOffset(IoBuffer);
    FragmentIndex = 0;
    FragmentOffset = 0;
    while (IoBufferOffset != 0) {

        ASSERT(FragmentIndex < IoBuffer->FragmentCount);

        Fragment = &(IoBuffer->Fragment[FragmentIndex]);
        if (IoBufferOffset < Fragment->Size) {
            FragmentOffset = IoBufferOffset;
            break;
        }

        IoBufferOffset -= Fragment->Size;
        FragmentIndex += 1;
    }

    if (LockRequired != FALSE) {
        KeAcquireQueuedLock(Disk->ControllerLock);
        LockHeld = TRUE;
    }

    if (Disk->MediaPresent == FALSE) {
        Status = STATUS_NO_MEDIA;
        goto PerformBlockIoPolledEnd;
    }

    //
    // Loop reading in or writing out each fragment in the I/O buffer.
    //

    BlockOffset = BlockAddress;
    while (BlocksComplete != BlocksToComplete) {

        ASSERT(FragmentIndex < IoBuffer->FragmentCount);

        Fragment = (PIO_BUFFER_FRAGMENT)&(IoBuffer->Fragment[FragmentIndex]);
        VirtualAddress = Fragment->VirtualAddress + FragmentOffset;
        PhysicalAddress = Fragment->PhysicalAddress + FragmentOffset;
        FragmentSize = Fragment->Size - FragmentOffset;

        ASSERT(IS_ALIGNED(PhysicalAddress, (1 << Disk->BlockShift)) != FALSE);
        ASSERT(IS_ALIGNED(FragmentSize, (1 << Disk->BlockShift)) != FALSE);

        BlockCount = FragmentSize >> Disk->BlockShift;
        if ((BlocksToComplete - BlocksComplete) < BlockCount) {
            BlockCount = BlocksToComplete - BlocksComplete;
        }

        //
        // Make sure the system isn't trying to do I/O off the end of the disk.
        //

        ASSERT(BlockOffset < Disk->BlockCount);
        ASSERT(BlockCount >= 1);

        Status = SdBlockIoPolled(Disk->Controller,
                                 BlockOffset,
                                 BlockCount,
                                 VirtualAddress,
                                 Write);

        if (!KSUCCESS(Status)) {
            goto PerformBlockIoPolledEnd;
        }

        BlockOffset += BlockCount;
        BlocksComplete += BlockCount;
        FragmentOffset += BlockCount << Disk->BlockShift;
        if (FragmentOffset >= Fragment->Size) {
            FragmentIndex += 1;
            FragmentOffset = 0;
        }
    }

    Status = STATUS_SUCCESS;

PerformBlockIoPolledEnd:
    if (LockHeld != FALSE) {
        KeReleaseQueuedLock(Disk->ControllerLock);
    }

    //
    // Free the buffer used for I/O if it differs from the original.
    //

    if (OriginalIoBuffer != IoBuffer) {

        //
        // On a read operation, potentially copy the data back into the
        // original I/O buffer.
        //

        if ((Write == FALSE) && (BlocksComplete != 0)) {
            Status = MmCopyIoBuffer(OriginalIoBuffer,
                                    0,
                                    IoBuffer,
                                    0,
                                    BlocksComplete << Disk->BlockShift);

            if (!KSUCCESS(Status)) {
                BlocksComplete = 0;
            }
        }

        MmFreeIoBuffer(IoBuffer);
    }

    //
    // For polled reads, the data must be brought to the point of
    // unification in case it is to be executed. This responsibility is
    // pushed on the driver because DMA does not need to do it and the
    // kernel does not know whether an individual read was done with DMA or
    // not. The downside is that data regions also get flushed, and not
    // just the necessary code regions.
    //

    if ((Write == FALSE) && (BlocksComplete != 0)) {
        for (FragmentIndex = 0;
             FragmentIndex < OriginalIoBuffer->FragmentCount;
             FragmentIndex += 1) {

            Fragment = &(OriginalIoBuffer->Fragment[FragmentIndex]);
            MmFlushBuffer(Fragment->VirtualAddress, Fragment->Size);
        }
    }

    *BlocksCompleted = BlocksComplete;
    return Status;
}

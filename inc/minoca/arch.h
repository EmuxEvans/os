/*++

Copyright (c) 2012 Minoca Corp. All Rights Reserved

Module Name:

    arch.h

Abstract:

    This header contains definitions for architecture dependent but universally
    required functionality.

Author:

    Evan Green 10-Aug-2012

--*/

//
// ------------------------------------------------------------------- Includes
//

//
// ---------------------------------------------------------------- Definitions
//

#define ARCH_POOL_TAG 0x68637241 // 'hcrA'

//
// ------------------------------------------------------ Data Type Definitions
//

typedef struct _TRAP_FRAME TRAP_FRAME, *PTRAP_FRAME;
typedef struct _FPU_CONTEXT FPU_CONTEXT, *PFPU_CONTEXT;

//
// -------------------------------------------------------------------- Globals
//

//
// -------------------------------------------------------- Function Prototypes
//

ULONG
ArGetDataCacheLineSize (
    VOID
    );

/*++

Routine Description:

    This routine gets the size of a line in the L1 data cache.

Arguments:

    None.

Return Value:

    Returns the L1 data cache line size, in bytes.

--*/

VOID
ArCleanCacheRegion (
    PVOID Address,
    UINTN Size
    );

/*++

Routine Description:

    This routine cleans the given region of virtual address space in the first
    level data cache.

Arguments:

    Address - Supplies the virtual address of the region to clean.

    Size - Supplies the number of bytes to clean.

Return Value:

    None.

--*/

VOID
ArCleanInvalidateCacheRegion (
    PVOID Address,
    UINTN Size
    );

/*++

Routine Description:

    This routine cleans and invalidates the given region of virtual address
    space in the first level data cache.

Arguments:

    Address - Supplies the virtual address of the region to clean.

    Size - Supplies the number of bytes to clean.

Return Value:

    None.

--*/

VOID
ArInvalidateCacheRegion (
    PVOID Address,
    UINTN Size
    );

/*++

Routine Description:

    This routine invalidates the region of virtual address space in the first
    level data cache. This routine is very dangerous, as any dirty data in the
    cache will be lost and gone.

Arguments:

    Address - Supplies the virtual address of the region to clean.

    Size - Supplies the number of bytes to clean.

Return Value:

    None.

--*/

VOID
ArInitializeProcessor (
    BOOL PhysicalMode,
    PVOID ProcessorStructures
    );

/*++

Routine Description:

    This routine initializes processor-specific structures.

Arguments:

    PhysicalMode - Supplies a boolean indicating whether or not the processor
        is operating in physical mode.

    ProcessorStructures - Supplies a pointer to the memory to use for basic
        processor structures, as returned by the allocate processor structures
        routine. For the boot processor, supply NULL here to use this routine's
        internal resources.

Return Value:

    None.

--*/

KSTATUS
ArFinishBootProcessorInitialization (
    VOID
    );

/*++

Routine Description:

    This routine performs additional initialization steps for processor 0 that
    were put off in pre-debugger initialization.

Arguments:

    None.

Return Value:

    Status code.

--*/

PVOID
ArAllocateProcessorStructures (
    ULONG ProcessorNumber
    );

/*++

Routine Description:

    This routine attempts to allocate and initialize early structures needed by
    a new processor.

Arguments:

    ProcessorNumber - Supplies the number of the processor that these resources
        will go to.

Return Value:

    Returns a pointer to the new processor resources on success.

    NULL on failure.

--*/

VOID
ArFreeProcessorStructures (
    PVOID ProcessorStructures
    );

/*++

Routine Description:

    This routine destroys a set of processor structures that have been
    allocated. It should go without saying, but obviously a processor must not
    be actively using these resources.

Arguments:

    ProcessorStructures - Supplies the pointer returned by the allocation
        routine.

Return Value:

    None.

--*/

BOOL
ArIsTranslationEnabled (
    VOID
    );

/*++

Routine Description:

    This routine determines if the processor was initialized with virtual-to-
    physical address translation enabled or not.

Arguments:

    None.

Return Value:

    TRUE if the processor is using a layer of translation between CPU accessible
    addresses and physical memory.

    FALSE if the processor was initialized in physical mode.

--*/

ULONG
ArGetIoPortCount (
    VOID
    );

/*++

Routine Description:

    This routine returns the number of I/O port addresses architecturally
    available.

Arguments:

    None.

Return Value:

    Returns the number of I/O port address supported by the architecture.

--*/

ULONG
ArGetInterruptVectorCount (
    VOID
    );

/*++

Routine Description:

    This routine returns the number of interrupt vectors in the system, either
    architecturally defined or artificially created.

Arguments:

    None.

Return Value:

    Returns the number of interrupt vectors in use by the system.

--*/

ULONG
ArGetMinimumDeviceVector (
    VOID
    );

/*++

Routine Description:

    This routine returns the first interrupt vector that can be used by
    devices.

Arguments:

    None.

Return Value:

    Returns the minimum interrupt vector available for use by devices.

--*/

ULONG
ArGetMaximumDeviceVector (
    VOID
    );

/*++

Routine Description:

    This routine returns the last interrupt vector that can be used by
    devices.

Arguments:

    None.

Return Value:

    Returns the maximum interrupt vector available for use by devices.

--*/

ULONG
ArGetTrapFrameSize (
    VOID
    );

/*++

Routine Description:

    This routine returns the size of the trap frame structure, in bytes.

Arguments:

    None.

Return Value:

    Returns the size of the trap frame structure, in bytes.

--*/

PVOID
ArGetInstructionPointer (
    PTRAP_FRAME TrapFrame
    );

/*++

Routine Description:

    This routine returns the instruction pointer out of the trap frame.

Arguments:

    TrapFrame - Supplies the trap frame from which the instruction pointer
        will be returned.

Return Value:

    Returns the instruction pointer the trap frame is pointing to.

--*/

BOOL
ArIsTrapFrameFromPrivilegedMode (
    PTRAP_FRAME TrapFrame
    );

/*++

Routine Description:

    This routine determines if the given trap frame occurred in a privileged
    environment or not.

Arguments:

    TrapFrame - Supplies the trap frame.

Return Value:

    TRUE if the execution environment of the trap frame is privileged.

    FALSE if the execution environment of the trap frame is not privileged.

--*/

VOID
ArSetSingleStep (
    PTRAP_FRAME TrapFrame
    );

/*++

Routine Description:

    This routine modifies the given trap frame registers so that a single step
    exception will occur. This is only supported on some architectures.

Arguments:

    TrapFrame - Supplies a pointer to the trap frame not modify.

Return Value:

    None.

--*/

VOID
ArInvalidateInstructionCacheRegion (
    PVOID Address,
    ULONG Size
    );

/*++

Routine Description:

    This routine invalidates the given region of virtual address space in the
    instruction cache.

Arguments:

    Address - Supplies the virtual address of the region to invalidate.

    Size - Supplies the number of bytes to invalidate.

Return Value:

    None.

--*/

BOOL
ArAreInterruptsEnabled (
    VOID
    );

/*++

Routine Description:

    This routine determines whether or not interrupts are currently enabled
    on the processor.

Arguments:

    None.

Return Value:

    TRUE if interrupts are enabled in the processor.

    FALSE if interrupts are globally disabled.

--*/

BOOL
ArDisableInterrupts (
    VOID
    );

/*++

Routine Description:

    This routine disables all interrupts on the current processor.

Arguments:

    None.

Return Value:

    TRUE if interrupts were previously enabled.

    FALSE if interrupts were not previously enabled.

--*/

VOID
ArEnableInterrupts (
    VOID
    );

/*++

Routine Description:

    This routine enables interrupts on the current processor.

Arguments:

    None.

Return Value:

    None.

--*/

ULONG
ArGetProcessorFlags (
    VOID
    );

/*++

Routine Description:

    This routine gets the current processor's flags register.

Arguments:

    None.

Return Value:

    Returns the current flags.

--*/

VOID
ArInvalidateTlbEntry (
    PVOID Address
    );

/*++

Routine Description:

    This routine invalidates one TLB entry corresponding to the given virtual
    address.

Arguments:

    Address - Supplies the virtual address whose associated TLB entry will be
        invalidated.

Return Value:

    None.

--*/

VOID
ArInvalidateEntireTlb (
    VOID
    );

/*++

Routine Description:

    This routine invalidates the entire TLB.

Arguments:

    None.

Return Value:

    None.

--*/

VOID
ArProcessorYield (
    VOID
    );

/*++

Routine Description:

    This routine executes a short processor yield in hardware.

Arguments:

    None.

Return Value:

    None.

--*/

VOID
ArWaitForInterrupt (
    VOID
    );

/*++

Routine Description:

    This routine halts the processor until the next interrupt comes in. This
    routine should be called with interrupts disabled, and will return with
    interrupts enabled.

Arguments:

    None.

Return Value:

    None.

--*/

VOID
ArSerializeExecution (
    VOID
    );

/*++

Routine Description:

    This routine acts a serializing instruction, preventing the processor
    from speculatively executing beyond this point.

Arguments:

    None.

Return Value:

    None.

--*/

VOID
ArInvalidateInstructionCache (
    VOID
    );

/*++

Routine Description:

    This routine invalidate the processor's instruction only cache, indicating
    that a page containing code has changed.

Arguments:

    None.

Return Value:

    None.

--*/

VOID
ArSetUpUserSharedDataFeatures (
    VOID
    );

/*++

Routine Description:

    This routine initialize the user shared data processor specific features.

Arguments:

    None.

Return Value:

    None.

--*/

PFPU_CONTEXT
ArAllocateFpuContext (
    ULONG AllocationTag
    );

/*++

Routine Description:

    This routine allocates a buffer that can be used for FPU context.

Arguments:

    AllocationTag - Supplies the pool allocation tag to use for the allocation.

Return Value:

    Returns a pointer to the newly allocated FPU context on success.

    NULL on allocation failure.

--*/

VOID
ArDestroyFpuContext (
    PFPU_CONTEXT Context
    );

/*++

Routine Description:

    This routine destroys a previously allocated FPU context buffer.

Arguments:

    Context - Supplies a pointer to the context to destroy.

Return Value:

    None.

--*/

VOID
ArSetThreadPointer (
    PVOID Thread,
    PVOID NewThreadPointer
    );

/*++

Routine Description:

    This routine sets the new thread pointer value.

Arguments:

    Thread - Supplies a pointer to the thread to set the thread pointer for.

    NewThreadPointer - Supplies the new thread pointer value to set.

Return Value:

    None.

--*/

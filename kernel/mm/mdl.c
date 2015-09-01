/*++

Copyright (c) 2012 Minoca Corp. All Rights Reserved

Module Name:

    mdl.c

Abstract:

    This module contains utility functions for manipulating MDLs (memory
    descriptor lists).

Author:

    Evan Green 27-Jul-2012

Environment:

    Kernel, Boot

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include "mmp.h"

//
// --------------------------------------------------------------------- Macros
//

//
// ---------------------------------------------------------------- Definitions
//

#define DESCRIPTOR_BATCH 0x20
#define MDL_PRINT RtlDebugPrint

//
// Define the number of bits to shift the descriptor to get a bin index.
//

#define MDL_BIN_SHIFT 12

//
// ----------------------------------------------- Internal Function Prototypes
//

PMEMORY_DESCRIPTOR
MmpMdFindDescriptor (
    PMEMORY_DESCRIPTOR_LIST DescriptorList,
    ULONGLONG BaseAddress
    );

VOID
MmpMdAddFreeDescriptor (
    PMEMORY_DESCRIPTOR_LIST DescriptorList,
    PMEMORY_DESCRIPTOR Descriptor
    );

PSTR
MmpMdPrintMemoryType (
    MEMORY_TYPE MemoryType
    );

PMEMORY_DESCRIPTOR
MmpMdAllocateDescriptor (
    PMEMORY_DESCRIPTOR_LIST Mdl
    );

VOID
MmpMdDestroyIterationRoutine (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE Node,
    ULONG Level,
    PVOID Context
    );

VOID
MmpMdPrintIterationRoutine (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE Node,
    ULONG Level,
    PVOID Context
    );

VOID
MmpMdIterationRoutine (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE Node,
    ULONG Level,
    PVOID Context
    );

COMPARISON_RESULT
MmpMdCompareDescriptors (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE FirstNode,
    PRED_BLACK_TREE_NODE SecondNode
    );

UINTN
MmpMdGetFreeBinIndex (
    ULONGLONG Size
    );

//
// ------------------------------------------------------ Data Type Definitions
//

/*++

Structure Description:

    This structure defines the iteration context used when destroying a
    memory descriptor list.

Members:

    Mdl - Stores a pointer to the MDL.

    FreeList - Stores the list of descriptor allocations to free.

--*/

typedef struct _MDL_DESTROY_CONTEXT {
    PMEMORY_DESCRIPTOR_LIST Mdl;
    LIST_ENTRY FreeList;
} MDL_DESTROY_CONTEXT, *PMDL_DESTROY_CONTEXT;

/*++

Structure Description:

    This structure defines the iteration context used when printing a
    descriptor list.

Members:

    Mdl - Stores a pointer to the MDL.

    DescriptorCount - Stores the total descriptor count.

    TotalSpace - Stores the total amount of space described by the descriptor
        list.

    TotalFree - Stores the total amount of free space described by the
        descriptor list.

    PreviousEnd - Stores the end address of the last visited node.

--*/

typedef struct _MDL_PRINT_CONTEXT {
    PMEMORY_DESCRIPTOR_LIST Mdl;
    UINTN DescriptorCount;
    ULONGLONG TotalSpace;
    ULONGLONG TotalFree;
    ULONGLONG PreviousEnd;
} MDL_PRINT_CONTEXT, *PMDL_PRINT_CONTEXT;

/*++

Structure Description:

    This structure defines the iteration context used when performing an
    iteration for someone outside the MDL library.

Members:

    Mdl - Stores a pointer to the MDL.

    IterationRoutine - Stores the iteration routine to call.

    Context - Stores the context to pass to the iteration routine.

--*/

typedef struct _MDL_ITERATE_CONTEXT {
    PMEMORY_DESCRIPTOR_LIST Mdl;
    PMEMORY_DESCRIPTOR_LIST_ITERATION_ROUTINE IterationRoutine;
    PVOID Context;
} MDL_ITERATE_CONTEXT, *PMDL_ITERATE_CONTEXT;

//
// -------------------------------------------------------------------- Globals
//

//
// ------------------------------------------------------------------ Functions
//

VOID
MmMdInitDescriptorList (
    PMEMORY_DESCRIPTOR_LIST Mdl,
    MDL_ALLOCATION_SOURCE AllocationSource
    )

/*++

Routine Description:

    This routine initializes a memory descriptor list.

Arguments:

    Mdl - Supplies a pointer to the MDL to initialize.

    AllocationSource - Supplies the way that additional descriptors should be
        allocated.

Return Value:

    None.

--*/

{

    UINTN Bin;

    RtlRedBlackTreeInitialize(&(Mdl->Tree), 0, MmpMdCompareDescriptors);
    for (Bin = 0; Bin < MDL_BIN_COUNT; Bin += 1) {
        INITIALIZE_LIST_HEAD(&(Mdl->FreeLists[Bin]));
    }

    INITIALIZE_LIST_HEAD(&(Mdl->UnusedListHead));
    Mdl->DescriptorCount = 0;
    Mdl->AllocationSource = AllocationSource;
    Mdl->UnusedDescriptorCount = 0;
    Mdl->TotalSpace = 0;
    Mdl->FreeSpace = 0;
    return;
}

VOID
MmMdDestroyDescriptorList (
    PMEMORY_DESCRIPTOR_LIST Mdl
    )

/*++

Routine Description:

    This routine destroys a memory descriptor list. It frees all descriptors.

Arguments:

    Mdl - Supplies a pointer to the MDL to destroy.

Return Value:

    None.

--*/

{

    MDL_DESTROY_CONTEXT Context;
    PMEMORY_DESCRIPTOR Descriptor;
    PLIST_ENTRY Entry;

    Context.Mdl = Mdl;
    INITIALIZE_LIST_HEAD(&(Context.FreeList));

    //
    // Iterate through the descriptors and move any that can be freed onto the
    // free list.
    //

    RtlRedBlackTreeIterate(&(Mdl->Tree),
                           MmpMdDestroyIterationRoutine,
                           &Context);

    //
    // Do the same for the free descriptor cache.
    //

    while (LIST_EMPTY(&(Mdl->UnusedListHead)) == FALSE) {
        Entry = Mdl->UnusedListHead.Next;
        LIST_REMOVE(Entry);
        Mdl->UnusedDescriptorCount -= 1;
        Descriptor = LIST_VALUE(Entry, MEMORY_DESCRIPTOR, FreeListEntry);
        Descriptor->Flags &= ~DESCRIPTOR_FLAG_USED;
        if ((Descriptor->Flags & DESCRIPTOR_FLAG_FREEABLE) != 0) {
            INSERT_BEFORE(Entry, &(Context.FreeList));
        }
    }

    ASSERT(Mdl->UnusedDescriptorCount == 0);

    //
    // Reclaim everything on the free list.
    //

    while (LIST_EMPTY(&(Context.FreeList)) == FALSE) {
        Entry = Context.FreeList.Next;
        LIST_REMOVE(Entry);
        Descriptor = LIST_VALUE(Entry, MEMORY_DESCRIPTOR, FreeListEntry);

        ASSERT((Descriptor->Flags & DESCRIPTOR_FLAG_FREEABLE) != 0);

        switch (Mdl->AllocationSource) {

        //
        // If there was no allocation source, the descriptors came from
        // somewheres unknown.
        //

        case MdlAllocationSourceNone:
            break;

        //
        // Free non-paged pool allocations.
        //

        case MdlAllocationSourceNonPagedPool:
            MmFreeNonPagedPool(Descriptor);
            break;

        //
        // Free paged pool allocations.
        //

        case MdlAllocationSourcePagedPool:
            MmFreePagedPool(Descriptor);
            break;

        default:

            ASSERT(FALSE);

            break;
        }
    }

    Mdl->TotalSpace = 0;
    Mdl->FreeSpace = 0;
    return;
}

VOID
MmMdInitDescriptor (
    PMEMORY_DESCRIPTOR Descriptor,
    ULONGLONG MinimumAddress,
    ULONGLONG MaximumAddress,
    MEMORY_TYPE Type
    )

/*++

Routine Description:

    This routine initializes a memory descriptor. Unaligned addresses are
    expanded out to page boundaries.

Arguments:

    Descriptor - Supplies a pointer to the uninitialized descriptor.

    MinimumAddress - Supplies the base address of the descriptor.

    MaximumAddress - Supplies the top address of the descriptor. This is the
        first address NOT described by the descriptor.

    Type - Supplies the memory type of the descriptor.

Return Value:

    None.

--*/

{

    Descriptor->BaseAddress = MinimumAddress;
    Descriptor->Size = MaximumAddress - MinimumAddress;

    ASSERT(Descriptor->Size != 1);
    ASSERT((Descriptor->Size & 0xFFF) == 0);

    Descriptor->Type = Type;
    return;
}

KSTATUS
MmMdAddDescriptorToList (
    PMEMORY_DESCRIPTOR_LIST Mdl,
    PMEMORY_DESCRIPTOR NewDescriptor
    )

/*++

Routine Description:

    This routine adds the given descriptor to the descriptor list, regardless
    of what other descriptors are currently describing that region. This
    routine is useful for overriding regions described incorrectly by the
    firmware.

Arguments:

    Mdl - Supplies a pointer to the destination descriptor list the descriptor
        should be added to.

    NewDescriptor - Supplies a pointer to the descriptor to be added.

Return Value:

    Status code.

--*/

{

    BOOL Added;
    PMEMORY_DESCRIPTOR AllocatedDescriptor;
    ULONGLONG CurrentAddress;
    ULONGLONG EndAddress;
    PMEMORY_DESCRIPTOR Existing;
    ULONGLONG ExistingBase;
    PMEMORY_DESCRIPTOR Next;
    ULONGLONG NextAddress;
    PRED_BLACK_TREE_NODE NextNode;
    ULONGLONG Reduction;
    KSTATUS Status;

    //
    // The new descriptor better not overflow or have a zero size.
    //

    ASSERT(NewDescriptor->BaseAddress + NewDescriptor->Size >
           NewDescriptor->BaseAddress);

    EndAddress = NewDescriptor->BaseAddress + NewDescriptor->Size;
    CurrentAddress = EndAddress - 1;
    Existing = NULL;

    //
    // Loop making sure the range is clear, starting from the end.
    //

    while (CurrentAddress + 1 >= NewDescriptor->BaseAddress) {
        Existing = MmpMdFindDescriptor(Mdl, CurrentAddress);

        //
        // If there is no descriptor for this address or lower, allocate and
        // add the descriptor.
        //

        if ((Existing == NULL) ||
            (Existing->BaseAddress + Existing->Size <=
             NewDescriptor->BaseAddress)) {

            break;
        }

        ExistingBase = Existing->BaseAddress;
        NextAddress = ExistingBase - 1;

        //
        // If the descriptor goes off the end, clip it. This does not change
        // the ordering in the tree since there are no overlapping regions.
        //

        if ((ExistingBase >= NewDescriptor->BaseAddress) &&
            (ExistingBase + Existing->Size > EndAddress)) {

            Reduction = EndAddress - Existing->BaseAddress;
            Existing->BaseAddress = EndAddress;
            Existing->Size -= Reduction;
            Mdl->TotalSpace -= Reduction;
            if (IS_MEMORY_FREE_TYPE(Existing->Type)) {
                Mdl->FreeSpace -= Reduction;
                LIST_REMOVE(&(Existing->FreeListEntry));
                MmpMdAddFreeDescriptor(Mdl, Existing);
            }

            Existing = NULL;

        //
        // If the existing descriptor is completely inside the new one, remove
        // it.
        //

        } else if ((Existing->BaseAddress >= NewDescriptor->BaseAddress) &&
                   (Existing->BaseAddress + Existing->Size <= EndAddress)) {

            MmMdRemoveDescriptorFromList(Mdl, Existing);
            Existing = NULL;

        //
        // The existing descriptor must start before the new descriptor.
        //

        } else {

            ASSERT(ExistingBase < NewDescriptor->BaseAddress);

            //
            // If the existing descriptor completely contains the new one, then
            // either split it, or just return successfully if they are the
            // same type.
            //

            if (ExistingBase + Existing->Size > EndAddress) {
                if (Existing->Type == NewDescriptor->Type) {
                    Status = STATUS_SUCCESS;
                    goto AddDescriptorToListEnd;
                }

                //
                // Create the split one for the end.
                //

                AllocatedDescriptor = MmpMdAllocateDescriptor(Mdl);
                if (AllocatedDescriptor == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto AddDescriptorToListEnd;
                }

                Reduction = NewDescriptor->Size;
                AllocatedDescriptor->BaseAddress = EndAddress;
                AllocatedDescriptor->Size = ExistingBase + Existing->Size -
                                            EndAddress;

                AllocatedDescriptor->Type = Existing->Type;
                Existing->Size = NewDescriptor->BaseAddress -
                                 ExistingBase;

                if (IS_MEMORY_FREE_TYPE(Existing->Type)) {
                    LIST_REMOVE(&(Existing->FreeListEntry));
                    MmpMdAddFreeDescriptor(Mdl, Existing);
                }

                RtlRedBlackTreeInsert(&(Mdl->Tree),
                                      &(AllocatedDescriptor->TreeNode));

                Mdl->DescriptorCount += 1;
                Mdl->TotalSpace -= Reduction;
                if (IS_MEMORY_FREE_TYPE(AllocatedDescriptor->Type)) {
                    MmpMdAddFreeDescriptor(Mdl, AllocatedDescriptor);
                    Mdl->FreeSpace -= Reduction;
                }

            //
            // The existing descriptor starts before but doesn't cover the new
            // one fully, so shrink the existing descriptor.
            //

            } else {
                Reduction = ExistingBase + Existing->Size -
                            NewDescriptor->BaseAddress;

                Existing->Size = NewDescriptor->BaseAddress - ExistingBase;
                Mdl->TotalSpace -= Reduction;
                if (IS_MEMORY_FREE_TYPE(Existing->Type)) {
                    Mdl->FreeSpace -= Reduction;
                    LIST_REMOVE(&(Existing->FreeListEntry));
                    MmpMdAddFreeDescriptor(Mdl, Existing);
                }
            }

            break;
        }

        //
        // If this was the minimum possible value, don't wrap.
        //

        if (ExistingBase == 0) {
            NextAddress = 0;
            break;
        }

        CurrentAddress = NextAddress;
    }

    //
    // Coalesce with the previous descriptor if there was one.
    //

    Added = FALSE;
    Next = NULL;
    if (Existing != NULL) {

        //
        // Get the next after the previous, which may coalesce with the end
        // of the new descriptor.
        //

        NextNode = RtlRedBlackTreeGetNextNode(&(Mdl->Tree),
                                              FALSE,
                                              &(Existing->TreeNode));

        if (NextNode != NULL) {
            Next = RED_BLACK_TREE_VALUE(NextNode, MEMORY_DESCRIPTOR, TreeNode);
        }

        if ((Existing->Type == NewDescriptor->Type) &&
            (Existing->BaseAddress + Existing->Size ==
             NewDescriptor->BaseAddress)) {

            Mdl->TotalSpace += NewDescriptor->Size;
            Existing->Size += NewDescriptor->Size;
            if (IS_MEMORY_FREE_TYPE(Existing->Type)) {
                Mdl->FreeSpace += NewDescriptor->Size;
                LIST_REMOVE(&(Existing->FreeListEntry));
                MmpMdAddFreeDescriptor(Mdl, Existing);
            }

            Added = TRUE;

            //
            // If the next one coalesces as well, remove it. Add to the globals
            // since the remove routine is going to subtract.
            //

            if ((Next != NULL) &&
                (Next->Type == Existing->Type) &&
                (EndAddress == Next->BaseAddress)) {

                Existing->Size += Next->Size;
                Mdl->TotalSpace += Next->Size;
                if (IS_MEMORY_FREE_TYPE(Existing->Type)) {
                    Mdl->FreeSpace += Next->Size;
                    LIST_REMOVE(&(Existing->FreeListEntry));
                    MmpMdAddFreeDescriptor(Mdl, Existing);
                }

                MmMdRemoveDescriptorFromList(Mdl, Next);
                Next = NULL;
            }
        }

    } else {
        Next = MmpMdFindDescriptor(Mdl, EndAddress);
    }

    //
    // This descriptor did not coalesce with the previous. Look to see if it
    // can coalesce with the next.
    //

    if ((Added == FALSE) && (Next != NULL) &&
        (Next->Type == NewDescriptor->Type) &&
        (EndAddress == Next->BaseAddress)) {

        Next->BaseAddress = NewDescriptor->BaseAddress;
        Next->Size += NewDescriptor->Size;
        Mdl->TotalSpace += NewDescriptor->Size;
        if (IS_MEMORY_FREE_TYPE(Next->Type)) {
            Mdl->FreeSpace += NewDescriptor->Size;
            LIST_REMOVE(&(Next->FreeListEntry));
            MmpMdAddFreeDescriptor(Mdl, Next);
        }

        Added = TRUE;
    }

    //
    // If the descriptor did not coalesce with any existing descriptors, add it
    // now.
    //

    if (Added == FALSE) {

        //
        // Add the new descriptor.
        //

        AllocatedDescriptor = MmpMdAllocateDescriptor(Mdl);
        if (AllocatedDescriptor == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto AddDescriptorToListEnd;
        }

        AllocatedDescriptor->BaseAddress = NewDescriptor->BaseAddress;
        AllocatedDescriptor->Size = NewDescriptor->Size;
        AllocatedDescriptor->Type = NewDescriptor->Type;
        RtlRedBlackTreeInsert(&(Mdl->Tree),
                              &(AllocatedDescriptor->TreeNode));

        Mdl->DescriptorCount += 1;
        Mdl->TotalSpace += AllocatedDescriptor->Size;
        if (IS_MEMORY_FREE_TYPE(AllocatedDescriptor->Type)) {
            MmpMdAddFreeDescriptor(Mdl, AllocatedDescriptor);
            Mdl->FreeSpace += AllocatedDescriptor->Size;
        }
    }

    Status = STATUS_SUCCESS;

AddDescriptorToListEnd:
    return Status;
}

PMEMORY_DESCRIPTOR
MmMdLookupDescriptor (
    PMEMORY_DESCRIPTOR_LIST Mdl,
    ULONGLONG StartAddress,
    ULONGLONG EndAddress
    )

/*++

Routine Description:

    This routine finds the memory descriptor corresponding to the given address.

Arguments:

    Mdl - Supplies a pointer to the descriptor list to search through.

    StartAddress - Supplies the first valid address of the region being
        queried for.

    EndAddress - Supplies the first address beyond the region being queried.
        In other words, the end address is not inclusive.

Return Value:

    Returns a pointer to the descriptor that covers the given address, or NULL
    if the address is not described by the list.

--*/

{

    PMEMORY_DESCRIPTOR Descriptor;

    Descriptor = MmpMdFindDescriptor(Mdl, EndAddress - 1);
    if (Descriptor != NULL) {
        if ((Descriptor->BaseAddress < EndAddress) &&
            (Descriptor->BaseAddress + Descriptor->Size > StartAddress)) {

            return Descriptor;
        }
    }

    return NULL;
}

PMEMORY_DESCRIPTOR
MmMdIsRangeFree (
    PMEMORY_DESCRIPTOR_LIST Mdl,
    ULONGLONG StartAddress,
    ULONGLONG EndAddress
    )

/*++

Routine Description:

    This routine determines if the given memory range is marked as free.

Arguments:

    Mdl - Supplies a pointer to the descriptor list to search through.

    StartAddress - Supplies the first valid address of the region being
        queried for.

    EndAddress - Supplies the first address beyond the region being queried.
        In other words, the end address is not inclusive.

Return Value:

    Returns a pointer to the descriptor with the free memory type that covers
    the given address range.

    NULL if entire specified range is not free.

--*/

{

    PMEMORY_DESCRIPTOR Descriptor;

    Descriptor = MmpMdFindDescriptor(Mdl, EndAddress - 1);
    if ((Descriptor == NULL) || (!IS_MEMORY_FREE_TYPE(Descriptor->Type))) {
        return NULL;
    }

    //
    // If the descriptor completely contains the region, return it.
    //

    if ((Descriptor->BaseAddress <= StartAddress) &&
        (Descriptor->BaseAddress + Descriptor->Size >= EndAddress)) {

        return Descriptor;
    }

    //
    // The range is not entirely free.
    //

    return NULL;
}

KSTATUS
MmMdRemoveRangeFromList (
    PMEMORY_DESCRIPTOR_LIST Mdl,
    ULONGLONG StartAddress,
    ULONGLONG EndAddress
    )

/*++

Routine Description:

    This routine removes all descriptors from the given list that are within
    the given memory range. Overlapping descriptors are truncated.

Arguments:

    Mdl - Supplies a pointer to the descriptor list to remove from.

    StartAddress - Supplies the first valid address of the region being removed.

    EndAddress - Supplies the first address beyond the region being removed.
        In other words, the end address is not inclusive.

Return Value:

    Status code.

--*/

{

    PMEMORY_DESCRIPTOR AllocatedDescriptor;
    ULONGLONG CurrentAddress;
    PMEMORY_DESCRIPTOR Existing;
    ULONGLONG ExistingBase;
    ULONGLONG Reduction;
    KSTATUS Status;

    ASSERT(StartAddress < EndAddress);

    CurrentAddress = EndAddress - 1;

    //
    // Loop removing descriptors from the range, starting from the end.
    //

    while (CurrentAddress >= StartAddress) {
        Existing = MmpMdFindDescriptor(Mdl, CurrentAddress);

        //
        // If there is no descriptor for this address or lower, then the work
        // is done.
        //

        if ((Existing == NULL) ||
            ((Existing->BaseAddress + Existing->Size) <= StartAddress)) {

            break;
        }

        ExistingBase = Existing->BaseAddress;

        //
        // If the descriptor goes off the end, clip it. This does not change
        // the ordering in the tree since there are no overlapping regions.
        //

        if ((ExistingBase >= StartAddress) &&
            ((ExistingBase + Existing->Size) > EndAddress)) {

            Reduction = EndAddress - Existing->BaseAddress;
            Existing->BaseAddress = EndAddress;
            Existing->Size -= Reduction;
            Mdl->TotalSpace -= Reduction;
            if (IS_MEMORY_FREE_TYPE(Existing->Type)) {
                Mdl->FreeSpace -= Reduction;
                LIST_REMOVE(&(Existing->FreeListEntry));
                MmpMdAddFreeDescriptor(Mdl, Existing);
            }

        //
        // If the existing descriptor is completely inside the range, remove it.
        //

        } else if ((Existing->BaseAddress >= StartAddress) &&
                   ((Existing->BaseAddress + Existing->Size) <= EndAddress)) {

            MmMdRemoveDescriptorFromList(Mdl, Existing);

        //
        // The existing descriptor must start before the memory range.
        //

        } else {

            ASSERT(ExistingBase < StartAddress);

            //
            // If the existing descriptor completely contains the range, then
            // split it.
            //

            if ((ExistingBase + Existing->Size) > EndAddress) {

                //
                // Create the split one for the end.
                //

                AllocatedDescriptor = MmpMdAllocateDescriptor(Mdl);
                if (AllocatedDescriptor == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto RemoveRangeFromListEnd;
                }

                Reduction = EndAddress - StartAddress;
                AllocatedDescriptor->BaseAddress = EndAddress;
                AllocatedDescriptor->Size = ExistingBase + Existing->Size -
                                            EndAddress;

                AllocatedDescriptor->Type = Existing->Type;
                Existing->Size = StartAddress - ExistingBase;
                if (IS_MEMORY_FREE_TYPE(Existing->Type)) {
                    LIST_REMOVE(&(Existing->FreeListEntry));
                    MmpMdAddFreeDescriptor(Mdl, Existing);
                }

                RtlRedBlackTreeInsert(&(Mdl->Tree),
                                      &(AllocatedDescriptor->TreeNode));

                Mdl->DescriptorCount += 1;
                Mdl->TotalSpace -= Reduction;
                if (IS_MEMORY_FREE_TYPE(AllocatedDescriptor->Type)) {
                    MmpMdAddFreeDescriptor(Mdl, AllocatedDescriptor);
                    Mdl->FreeSpace -= Reduction;
                }

            //
            // The existing descriptor starts before but doesn't cover the
            // range fully, so shrink the existing descriptor.
            //

            } else {
                Reduction = ExistingBase + Existing->Size - StartAddress;
                Existing->Size = StartAddress - ExistingBase;
                Mdl->TotalSpace -= Reduction;
                if (IS_MEMORY_FREE_TYPE(Existing->Type)) {
                    Mdl->FreeSpace -= Reduction;
                    LIST_REMOVE(&(Existing->FreeListEntry));
                    MmpMdAddFreeDescriptor(Mdl, Existing);
                }
            }

            break;
        }

        //
        // If this was the minimum possible value, don't wrap.
        //

        if (ExistingBase == 0) {
            break;
        }

        CurrentAddress = ExistingBase - 1;
    }

    Status = STATUS_SUCCESS;

RemoveRangeFromListEnd:
    return Status;
}

VOID
MmMdRemoveDescriptorFromList (
    PMEMORY_DESCRIPTOR_LIST Mdl,
    PMEMORY_DESCRIPTOR Descriptor
    )

/*++

Routine Description:

    This routine removes the given memory descriptor from the descriptor list.

Arguments:

    Mdl - Supplies a pointer to the descriptor list to remove from.

    Descriptor - Supplies a pointer to the descriptor to remove.

Return Value:

    None.

--*/

{

    RtlRedBlackTreeRemove(&(Mdl->Tree), &(Descriptor->TreeNode));
    Mdl->DescriptorCount -= 1;

    ASSERT(Mdl->TotalSpace >= Descriptor->Size);

    Mdl->TotalSpace -= Descriptor->Size;
    if (IS_MEMORY_FREE_TYPE(Descriptor->Type)) {
        LIST_REMOVE(&(Descriptor->FreeListEntry));
        Descriptor->FreeListEntry.Next = NULL;

        ASSERT(Mdl->FreeSpace >= Descriptor->Size);

        Mdl->FreeSpace -= Descriptor->Size;
    }

    INSERT_AFTER(&(Descriptor->FreeListEntry), &(Mdl->UnusedListHead));
    Mdl->UnusedDescriptorCount += 1;
    Descriptor->Flags &= ~DESCRIPTOR_FLAG_USED;
    return;
}

VOID
MmMdPrintMdl (
    PMEMORY_DESCRIPTOR_LIST Mdl
    )

/*++

Routine Description:

    This routine prints a memory descriptor list into a readable format.

Arguments:

    Mdl - Supplies a pointer to the descriptor list to print.

Return Value:

    None.

--*/

{

    MDL_PRINT_CONTEXT Context;

    RtlZeroMemory(&Context, sizeof(MDL_PRINT_CONTEXT));
    Context.Mdl = Mdl;
    MDL_PRINT("\n       Start Address    End Address  Size   Type\n");
    MDL_PRINT("-----------------------------------------------------------\n");
    RtlRedBlackTreeIterate(&(Mdl->Tree), MmpMdPrintIterationRoutine, &Context);
    MDL_PRINT("-----------------------------------------------------------\n");
    MDL_PRINT("Descriptor Count: %d  Free: 0x%I64x  Total: 0x%I64x\n\n",
              Mdl->DescriptorCount,
              Context.TotalFree,
              Context.TotalSpace);

    if (Context.DescriptorCount != Mdl->DescriptorCount) {
        MDL_PRINT("WARNING: The MDL claims there are %d descriptors, but %d "
                  "were described here!\n",
                  Mdl->DescriptorCount,
                  Context.DescriptorCount);

        ASSERT(FALSE);
    }

    if (Context.TotalSpace != Mdl->TotalSpace) {
        MDL_PRINT("WARNING: The MDL claims to have %I64x total space, "
                  "but %I64x total space was calculated.\n",
                  Mdl->TotalSpace,
                  Context.TotalSpace);

        ASSERT(FALSE);
    }

    if (Context.TotalFree != Mdl->FreeSpace) {
        MDL_PRINT("WARNING: The MDL claims to have %I64x free space, "
                  "but %I64x total space was calculated.\n",
                  Mdl->FreeSpace,
                  Context.TotalFree);

        ASSERT(FALSE);
    }

    return;
}

KSTATUS
MmMdAllocateFromMdl (
    PMEMORY_DESCRIPTOR_LIST Mdl,
    PULONGLONG Address,
    ULONGLONG Size,
    ULONG Alignment,
    MEMORY_TYPE MemoryType,
    ALLOCATION_STRATEGY Strategy
    )

/*++

Routine Description:

    This routine allocates a piece of free memory from the given descriptor
    list, and marks it as the given type in the list.

Arguments:

    Mdl - Supplies a pointer to the descriptor list to allocate memory from.

    Address - Supplies a pointer to where the allocation will be returned.

    Size - Supplies the size of the required space.

    Alignment - Supplies the alignment requirement for the allocation, in bytes.
        Valid values are powers of 2. Set to 1 or 0 to specify no alignment
        requirement.

    MemoryType - Supplies the type of memory to mark the allocation as.

    Strategy - Supplies the memory allocation strategy for this allocation.

Return Value:

    STATUS_SUCCESS if the allocation was successful.

    STATUS_INVALID_PARAMETER if a page count of 0 was passed or the address
        parameter was not filled out.

    STATUS_NO_MEMORY if the allocation request could not be filled.

--*/

{

    ULONGLONG AlignedAddress;
    ULONGLONG BestAlignedAddress;
    PMEMORY_DESCRIPTOR BestDescriptor;
    PLIST_ENTRY Bin;
    UINTN BinIndex;
    PLIST_ENTRY CurrentEntry;
    PMEMORY_DESCRIPTOR Descriptor;
    MEMORY_DESCRIPTOR Original;
    MEMORY_DESCRIPTOR Replacement;
    KSTATUS Status;

    ASSERT(Strategy < AllocationStrategyFixedAddress);

    if (Alignment == 1) {
        Alignment = 0;
    }

    BestAlignedAddress = 0;
    BestDescriptor = NULL;
    Original.Size = 0;

    //
    // Loop over each free bin, starting with the most appropriate size. If
    // allocating from the highest address, get it from the top of the largest
    // free block.
    //

    BinIndex = MmpMdGetFreeBinIndex(Size);
    if (Alignment == 0) {
        Alignment = 1;
    }

    if (Strategy == AllocationStrategyHighestAddress) {
        BinIndex = MDL_BIN_COUNT - 1;
        while ((BinIndex != 0) &&
               (LIST_EMPTY(&(Mdl->FreeLists[BinIndex])) != FALSE)) {

            BinIndex -= 1;
        }
    }

    while (BinIndex < MDL_BIN_COUNT) {
        Bin = &(Mdl->FreeLists[BinIndex]);
        BinIndex += 1;

        //
        // Loop over each entry in the bin, trying to find one big enough.
        //

        CurrentEntry = Bin->Next;
        while (CurrentEntry != Bin) {
            Descriptor = LIST_VALUE(CurrentEntry,
                                    MEMORY_DESCRIPTOR,
                                    FreeListEntry);

            CurrentEntry = CurrentEntry->Next;

            ASSERT(IS_MEMORY_FREE_TYPE(Descriptor->Type));

            if (Strategy == AllocationStrategyHighestAddress) {
                AlignedAddress = ALIGN_RANGE_DOWN(
                             Descriptor->BaseAddress + Descriptor->Size - Size,
                             Alignment);

            } else {
                AlignedAddress = ALIGN_RANGE_UP(Descriptor->BaseAddress,
                                                Alignment);
            }

            //
            // Skip it if it's not big enough or wraps in some weird way.
            //

            if ((AlignedAddress + Size >
                 Descriptor->BaseAddress + Descriptor->Size) ||
                (AlignedAddress < Descriptor->BaseAddress) ||
                (AlignedAddress + Size < Descriptor->BaseAddress) ||
                (AlignedAddress + Size < AlignedAddress)) {

                continue;
            }

            if (BestDescriptor == NULL) {
                BestDescriptor = Descriptor;
                BestAlignedAddress = AlignedAddress;

            } else {
                if (Strategy == AllocationStrategyLowestAddress) {
                    if (AlignedAddress < BestAlignedAddress) {
                        BestDescriptor = Descriptor;
                        BestAlignedAddress = AlignedAddress;
                    }

                } else if (Strategy == AllocationStrategyAnyAddress) {
                    break;

                } else if (Strategy == AllocationStrategyHighestAddress) {
                    if (AlignedAddress > BestAlignedAddress) {
                        BestDescriptor = Descriptor;
                        BestAlignedAddress = AlignedAddress;
                    }
                }
            }
        }

        //
        // In the case where the caller is not picky, this hopefully found
        // something suitable fast.
        //

        if ((BestDescriptor != NULL) &&
            (Strategy == AllocationStrategyAnyAddress)) {

            break;
        }
    }

    //
    // If the very best descriptor was found, sacrifice it.
    //

    if (BestDescriptor != NULL) {
        Descriptor = BestDescriptor;
        AlignedAddress = BestAlignedAddress;
        RtlCopyMemory(&Original, Descriptor, sizeof(MEMORY_DESCRIPTOR));

        //
        // After the descriptor is removed, it cannot be touched anymore,
        // as it's free for reuse. Make sure to use only the local copy.
        //

        MmMdRemoveDescriptorFromList(Mdl, Descriptor);
        Descriptor = NULL;

        //
        // Add the free sliver at the beginning if the alignment bumped
        // this up.
        //

        if (AlignedAddress != Original.BaseAddress) {
            MmMdInitDescriptor(&Replacement,
                               Original.BaseAddress,
                               AlignedAddress,
                               Original.Type);

            Status = MmMdAddDescriptorToList(Mdl, &Replacement);
            if (!KSUCCESS(Status)) {
                goto AllocateFromMdlEnd;
            }
        }

        //
        // Add the end chunk as well if this allocation doesn't cover it.
        //

        if (AlignedAddress + Size <
            Original.BaseAddress + Original.Size) {

            MmMdInitDescriptor(&Replacement,
                               AlignedAddress + Size,
                               Original.BaseAddress + Original.Size,
                               Original.Type);

            Status = MmMdAddDescriptorToList(Mdl, &Replacement);
            if (!KSUCCESS(Status)) {
                goto AllocateFromMdlEnd;
            }
        }

        //
        // Add the new allocation.
        //

        MmMdInitDescriptor(&Replacement,
                           AlignedAddress,
                           AlignedAddress + Size,
                           MemoryType);

        Status = MmMdAddDescriptorToList(Mdl, &Replacement);
        if (!KSUCCESS(Status)) {
            goto AllocateFromMdlEnd;
        }

        *Address = AlignedAddress;
        Status = STATUS_SUCCESS;

    //
    // No descriptor was found.
    //

    } else {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }

AllocateFromMdlEnd:
    if (!KSUCCESS(Status)) {

        //
        // Try to put the original back in place.
        //

        if (Original.Size != 0) {
            MmMdAddDescriptorToList(Mdl, &Original);
        }
    }

    return Status;
}

VOID
MmMdIterate (
    PMEMORY_DESCRIPTOR_LIST DescriptorList,
    PMEMORY_DESCRIPTOR_LIST_ITERATION_ROUTINE IterationRoutine,
    PVOID Context
    )

/*++

Routine Description:

    This routine iterates over all the descriptors in the given list, calling
    the iteration routine for each one.

Arguments:

    DescriptorList - Supplies a pointer to the list to iterate over.

    IterationRoutine - Supplies a pointer to the routine to call for each
        descriptor in the list.

    Context - Supplies an optional opaque context passed to the iteration
        routine.

Return Value:

    None.

--*/

{

    MDL_ITERATE_CONTEXT IterateContext;

    IterateContext.Mdl = DescriptorList;
    IterateContext.IterationRoutine = IterationRoutine;
    IterateContext.Context = Context;
    RtlRedBlackTreeIterate(&(DescriptorList->Tree),
                           MmpMdIterationRoutine,
                           &IterateContext);

    return;
}

VOID
MmMdAddFreeDescriptorsToMdl (
    PMEMORY_DESCRIPTOR_LIST Mdl,
    PMEMORY_DESCRIPTOR NewDescriptor,
    ULONG Size
    )

/*++

Routine Description:

    This routine adds new free descriptors to the given memory descriptor list.

Arguments:

    Mdl - Supplies a pointer to the descriptor list to add free descriptors to.

    NewDescriptor - Supplies an array of new descriptors.

    Size - Supplies the size of the descriptor array, in bytes.

Return Value:

    None.

--*/

{

    ULONG BytesUsed;
    PMEMORY_DESCRIPTOR CurrentDescriptor;

    BytesUsed = 0;
    CurrentDescriptor = NewDescriptor;
    while (BytesUsed + sizeof(MEMORY_DESCRIPTOR) <= Size) {
        CurrentDescriptor->Flags = 0;
        INSERT_BEFORE(&(CurrentDescriptor->FreeListEntry),
                      &(Mdl->UnusedListHead));

        Mdl->UnusedDescriptorCount += 1;
        BytesUsed += sizeof(MEMORY_DESCRIPTOR);
        CurrentDescriptor += 1;
    }

    return;
}

//
// --------------------------------------------------------- Internal Functions
//

PMEMORY_DESCRIPTOR
MmpMdFindDescriptor (
    PMEMORY_DESCRIPTOR_LIST DescriptorList,
    ULONGLONG BaseAddress
    )

/*++

Routine Description:

    This routine finds the descriptor containing the given base address, or
    the next lowest descriptor.

Arguments:

    DescriptorList - Supplies a pointer to the memory descriptor list.

    BaseAddress - Supplies the base address to find.

Return Value:

    Returns the descriptor containing the given base address, or the highest
    address lower than the given address.

    NULL if no descriptor covers the given address.

--*/

{

    PMEMORY_DESCRIPTOR Descriptor;
    PRED_BLACK_TREE_NODE Node;
    MEMORY_DESCRIPTOR Search;

    Descriptor = NULL;
    Search.BaseAddress = BaseAddress;
    Node = RtlRedBlackTreeSearchClosest(&(DescriptorList->Tree),
                                        &(Search.TreeNode),
                                        FALSE);

    if (Node != NULL) {
        Descriptor = RED_BLACK_TREE_VALUE(Node, MEMORY_DESCRIPTOR, TreeNode);
    }

    return Descriptor;
}

VOID
MmpMdAddFreeDescriptor (
    PMEMORY_DESCRIPTOR_LIST DescriptorList,
    PMEMORY_DESCRIPTOR Descriptor
    )

/*++

Routine Description:

    This routine links a descriptor in to the free list.

Arguments:

    DescriptorList - Supplies a pointer to the memory descriptor list.

    Descriptor - Supplies the descriptor to add to the free list entries.

Return Value:

    None.

--*/

{

    UINTN BinIndex;
    PLIST_ENTRY ListHead;

    BinIndex = MmpMdGetFreeBinIndex(Descriptor->Size);
    ListHead = &(DescriptorList->FreeLists[BinIndex]);
    INSERT_BEFORE(&(Descriptor->FreeListEntry), ListHead);
    return;
}

PSTR
MmpMdPrintMemoryType (
    MEMORY_TYPE MemoryType
    )

/*++

Routine Description:

    This routine returns a printable string associated with a memory type.

Arguments:

    MemoryType - Supplies the memory type.

Return Value:

    Returns a string describing the memory type.

--*/

{

    switch (MemoryType) {
    case MemoryTypeFree:
        return "Free Memory";

    case MemoryTypeReserved:
        return "Reserved";

    case MemoryTypeFirmwareTemporary:
        return "Firmware Temporary";

    case MemoryTypeFirmwarePermanent:
        return "Firmware Permanent";

    case MemoryTypeAcpiTables:
        return "ACPI Tables";

    case MemoryTypeAcpiNvStorage:
        return "ACPI Nonvolatile Storage";

    case MemoryTypeBad:
        return "Bad Memory";

    case MemoryTypeLoaderTemporary:
        return "Loader Temporary";

    case MemoryTypeLoaderPermanent:
        return "Loader Permanent";

    case MemoryTypePageTables:
        return "Page Tables";

    case MemoryTypeBootPageTables:
        return "Boot Page Tables";

    case MemoryTypeMmStructures:
        return "MM Init Structures";

    case MemoryTypeNonPagedPool:
        return "Non-paged Pool";

    case MemoryTypePagedPool:
        return "Paged Pool";

    case MemoryTypeHardware:
        return "Hardware";

    default:
        break;
    }

    return "Unknown Memory Type";
}

PMEMORY_DESCRIPTOR
MmpMdAllocateDescriptor (
    PMEMORY_DESCRIPTOR_LIST Mdl
    )

/*++

Routine Description:

    This routine allocates a new descriptor for use by the MDL. It will
    allocate from different means depending on the allocation strategy of the
    list.

Arguments:

    Mdl - Supplies a pointer to the MDL to allocate for.

Return Value:

    Returns a pointer to the newly allocated descriptor, or NULL if the
    descriptor could not be allocated.

--*/

{

    ULONG AllocationSize;
    PLIST_ENTRY Entry;
    PMEMORY_DESCRIPTOR NewDescriptor;

    AllocationSize = 0;
    NewDescriptor = NULL;

    //
    // If there are reserves left on the free list, use one of those.
    //

    if (Mdl->UnusedDescriptorCount != 0) {
        Entry = Mdl->UnusedListHead.Next;

        ASSERT(Entry != &(Mdl->UnusedListHead));

        LIST_REMOVE(Entry);
        Mdl->UnusedDescriptorCount -= 1;
        NewDescriptor = LIST_VALUE(Entry, MEMORY_DESCRIPTOR, FreeListEntry);
        NewDescriptor->Flags |= DESCRIPTOR_FLAG_USED;
        goto AllocateDescriptorEnd;
    }

    //
    // More descriptors need to be allocated.
    //

    switch (Mdl->AllocationSource) {

    //
    // With no allocation source, there's nothing that can be done.
    //

    case MdlAllocationSourceNone:

        ASSERT(FALSE);

        NewDescriptor = NULL;
        break;

    //
    // Allocate a batch of descriptors from non-paged pool.
    //

    case MdlAllocationSourceNonPagedPool:
        AllocationSize = sizeof(MEMORY_DESCRIPTOR) * DESCRIPTOR_BATCH;
        NewDescriptor = MmAllocateNonPagedPool(AllocationSize,
                                               MM_ALLOCATION_TAG);

        break;

    //
    // Allocate a batch of descriptors from the paged pool.
    //

    case MdlAllocationSourcePagedPool:
        AllocationSize = sizeof(MEMORY_DESCRIPTOR) * DESCRIPTOR_BATCH;
        NewDescriptor = MmAllocatePagedPool(AllocationSize, MM_ALLOCATION_TAG);
        break;

    //
    // Corrupt or uninitialized value.
    //

    default:

        ASSERT(FALSE);

        break;
    }

    //
    // If the allocation failed or was not big enough for one descriptor, fail.
    //

    if ((NewDescriptor == NULL) ||
        (AllocationSize < sizeof(MEMORY_DESCRIPTOR))) {

        NewDescriptor = NULL;
        goto AllocateDescriptorEnd;
    }

    //
    // Add all the new descriptors from the allocation into the free list.
    //

    MmMdAddFreeDescriptorsToMdl(Mdl, NewDescriptor, AllocationSize);

    //
    // Take the first one off the list and allocate it for the user. Mark
    // it as freeable since it was the beginning of this allocation.
    //

    LIST_REMOVE(&(NewDescriptor->FreeListEntry));
    Mdl->UnusedDescriptorCount -= 1;
    NewDescriptor->Flags |= DESCRIPTOR_FLAG_USED | DESCRIPTOR_FLAG_FREEABLE;

AllocateDescriptorEnd:
    if (NewDescriptor != NULL) {
        NewDescriptor->FreeListEntry.Next = NULL;
    }

    return NewDescriptor;
}

VOID
MmpMdDestroyIterationRoutine (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE Node,
    ULONG Level,
    PVOID Context
    )

/*++

Routine Description:

    This routine is called once for each node in the tree (via an in order
    traversal). It assumes that the tree will not be modified during the
    traversal.

Arguments:

    Tree - Supplies a pointer to the tree being enumerated.

    Node - Supplies a pointer to the node.

    Level - Supplies the depth into the tree that this node exists at. 0 is
        the root.

    Context - Supplies an optional opaque pointer of context that was provided
        when the iteration was requested.

Return Value:

    None.

--*/

{

    PMEMORY_DESCRIPTOR Descriptor;
    PMDL_DESTROY_CONTEXT DestroyContext;

    DestroyContext = Context;
    Descriptor = RED_BLACK_TREE_VALUE(Node, MEMORY_DESCRIPTOR, TreeNode);
    Descriptor->Flags &= ~DESCRIPTOR_FLAG_USED;
    if ((Descriptor->Flags & DESCRIPTOR_FLAG_FREEABLE) != 0) {
        INSERT_BEFORE(&(Descriptor->FreeListEntry),
                      &(DestroyContext->FreeList));
    }

    return;
}

VOID
MmpMdPrintIterationRoutine (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE Node,
    ULONG Level,
    PVOID Context
    )

/*++

Routine Description:

    This routine is called once for each node in the tree (via an in order
    traversal). It assumes that the tree will not be modified during the
    traversal.

Arguments:

    Tree - Supplies a pointer to the tree being enumerated.

    Node - Supplies a pointer to the node.

    Level - Supplies the depth into the tree that this node exists at. 0 is
        the root.

    Context - Supplies an optional opaque pointer of context that was provided
        when the iteration was requested.

Return Value:

    None.

--*/

{

    PMEMORY_DESCRIPTOR Descriptor;
    PMDL_PRINT_CONTEXT PrintContext;

    PrintContext = Context;
    Descriptor = RED_BLACK_TREE_VALUE(Node, MEMORY_DESCRIPTOR, TreeNode);
    MDL_PRINT("    %13I64x  %13I64x  %8I64x  %s\n",
              Descriptor->BaseAddress,
              Descriptor->BaseAddress + Descriptor->Size,
              Descriptor->Size,
              MmpMdPrintMemoryType(Descriptor->Type));

    PrintContext->DescriptorCount += 1;
    PrintContext->TotalSpace += Descriptor->Size;
    if (IS_MEMORY_FREE_TYPE(Descriptor->Type)) {
        PrintContext->TotalFree += Descriptor->Size;
    }

    if (Descriptor->BaseAddress < PrintContext->PreviousEnd) {
        MDL_PRINT("WARNING: Descriptor %x Base %I64x < PreviousEnd "
                  "%I64x.\n",
                  Descriptor,
                  Descriptor->BaseAddress,
                  PrintContext->PreviousEnd);

        ASSERT(FALSE);
    }

    PrintContext->PreviousEnd = Descriptor->BaseAddress + Descriptor->Size;
    return;
}

VOID
MmpMdIterationRoutine (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE Node,
    ULONG Level,
    PVOID Context
    )

/*++

Routine Description:

    This routine is called once for each node in the tree (via an in order
    traversal). It assumes that the tree will not be modified during the
    traversal.

Arguments:

    Tree - Supplies a pointer to the tree being enumerated.

    Node - Supplies a pointer to the node.

    Level - Supplies the depth into the tree that this node exists at. 0 is
        the root.

    Context - Supplies an optional opaque pointer of context that was provided
        when the iteration was requested.

Return Value:

    None.

--*/

{

    PMEMORY_DESCRIPTOR Descriptor;
    PMDL_ITERATE_CONTEXT IterateContext;

    IterateContext = Context;
    Descriptor = RED_BLACK_TREE_VALUE(Node, MEMORY_DESCRIPTOR, TreeNode);
    IterateContext->IterationRoutine(IterateContext->Mdl,
                                     Descriptor,
                                     IterateContext->Context);

    return;
}

COMPARISON_RESULT
MmpMdCompareDescriptors (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE FirstNode,
    PRED_BLACK_TREE_NODE SecondNode
    )

/*++

Routine Description:

    This routine compares two Red-Black tree nodes.

Arguments:

    Tree - Supplies a pointer to the Red-Black tree that owns both nodes.

    FirstNode - Supplies a pointer to the left side of the comparison.

    SecondNode - Supplies a pointer to the second side of the comparison.

Return Value:

    Same if the two nodes have the same value.

    Ascending if the first node is less than the second node.

    Descending if the second node is less than the first node.

--*/

{

    PMEMORY_DESCRIPTOR FirstDescriptor;
    PMEMORY_DESCRIPTOR SecondDescriptor;

    FirstDescriptor = RED_BLACK_TREE_VALUE(FirstNode,
                                           MEMORY_DESCRIPTOR,
                                           TreeNode);

    SecondDescriptor = RED_BLACK_TREE_VALUE(SecondNode,
                                            MEMORY_DESCRIPTOR,
                                            TreeNode);

    if (FirstDescriptor->BaseAddress < SecondDescriptor->BaseAddress) {
        return ComparisonResultAscending;

    } else if (FirstDescriptor->BaseAddress > SecondDescriptor->BaseAddress) {
        return ComparisonResultDescending;
    }

    return ComparisonResultSame;
}

UINTN
MmpMdGetFreeBinIndex (
    ULONGLONG Size
    )

/*++

Routine Description:

    This routine returns the free bin number for a given size.

Arguments:

    Size - Supplies the size of the region.

Return Value:

    Returns the appropriate free bin index for the given size.

--*/

{

    UINTN BinIndex;
    ULONGLONG BinSize;

    //
    // Round up to the nearest bin granularity, and shift down.
    //

    BinSize = (Size + ((1 << MDL_BIN_SHIFT) - 1)) >> MDL_BIN_SHIFT;
    BinIndex = ((UINTN)sizeof(ULONGLONG) * BITS_PER_BYTE) - 1 -
               RtlCountLeadingZeros64(BinSize);

    if (BinIndex >= MDL_BIN_COUNT) {
        BinIndex = MDL_BIN_COUNT - 1;
    }

    return BinIndex;
}

#include <ntddk.h>

// memory manager definitions for page tables

// TODO: 32bit
#ifndef _WIN64
#error 32-bit not supported
#endif

#define PTE_PER_PAGE_BITS 10    // This handles the case where the page is full

#define PDE_BASE          0xFFFFF6FB40000000UI64
#define PTE_BASE          0xFFFFF68000000000UI64

#define PTI_SHIFT 12
#define PDI_SHIFT 21

// Top level PXE mapping allocations:
//
// 0x0->0xF:        0x10 user entries
// 0x1ed:           0x1 for selfmaps
// 0x1ee:           0x1 hyperspace entry
// 0x1ef:           0x1 entry for syscache WSL & shared user data
// 0x1f0->0x1ff:    0x10 kernel entries

#define MM_MINIMUM_SYSTEM_PTES 7000
#define MM_MAXIMUM_SYSTEM_PTES (16*1024*1024)
#define MM_DEFAULT_SYSTEM_PTES 11000

#define _HARDWARE_PTE_WORKING_SET_BITS  11

#pragma warning(push)
#pragma warning(disable: 4214) // bitfields other than int

typedef struct _HARDWARE_PTE
{
    ULONG64 Valid : 1;
    ULONG64 Write : 1;                // UP version
    ULONG64 Owner : 1;
    ULONG64 WriteThrough : 1;
    ULONG64 CacheDisable : 1;
    ULONG64 Accessed : 1;
    ULONG64 Dirty : 1;
    ULONG64 LargePage : 1;
    ULONG64 Global : 1;
    ULONG64 CopyOnWrite : 1;          // software field
    ULONG64 Prototype : 1;            // software field
    ULONG64 reserved0 : 1;            // software field
    ULONG64 PageFrameNumber : 28;
    ULONG64 reserved1 : 24 - (_HARDWARE_PTE_WORKING_SET_BITS + 1);
    ULONG64 SoftwareWsIndex : _HARDWARE_PTE_WORKING_SET_BITS;
    ULONG64 NoExecute : 1;
} HARDWARE_PTE;

typedef struct _MMPTE_SOFTWARE
{
    ULONGLONG Valid : 1;
    ULONGLONG PageFileLow : 4;
    ULONGLONG Protection : 5;
    ULONGLONG Prototype : 1;
    ULONGLONG Transition : 1;
    ULONGLONG UsedPageTableEntries : PTE_PER_PAGE_BITS;
    ULONGLONG Reserved : 20 - PTE_PER_PAGE_BITS;
    ULONGLONG PageFileHigh : 32;
} MMPTE_SOFTWARE;

typedef struct _MMPTE_TRANSITION
{
    ULONGLONG Valid : 1;
    ULONGLONG Write : 1;
    ULONGLONG Owner : 1;
    ULONGLONG WriteThrough : 1;
    ULONGLONG CacheDisable : 1;
    ULONGLONG Protection : 5;
    ULONGLONG Prototype : 1;
    ULONGLONG Transition : 1;
    ULONGLONG PageFrameNumber : 28;
    ULONGLONG Unused : 24;
} MMPTE_TRANSITION;

typedef struct _MMPTE_PROTOTYPE
{
    ULONGLONG Valid : 1;
    ULONGLONG Unused0 : 7;
    ULONGLONG ReadOnly : 1;
    ULONGLONG Unused1 : 1;
    ULONGLONG Prototype : 1;
    ULONGLONG Protection : 5;
    LONGLONG ProtoAddress : 48;
} MMPTE_PROTOTYPE;

typedef struct _MMPTE_SUBSECTION
{
    ULONGLONG Valid : 1;
    ULONGLONG Unused0 : 4;
    ULONGLONG Protection : 5;
    ULONGLONG Prototype : 1;
    ULONGLONG Unused1 : 5;
    LONGLONG SubsectionAddress : 48;
} MMPTE_SUBSECTION;

typedef struct _MMPTE_LIST
{
    ULONGLONG Valid : 1;
    ULONGLONG OneEntry : 1;
    ULONGLONG filler0 : 3;

    //
    // Note the Prototype bit must not be used for lists like freed nonpaged
    // pool because lookaside pops can legitimately reference bogus addresses
    // (since the pop is unsynchronized) and the fault handler must be able to
    // distinguish lists from protos so a retry status can be returned (vs a
    // fatal bugcheck).
    //
    // The same caveat applies to both the Transition and the Protection
    // fields as they are similarly examined in the fault handler and would
    // be misinterpreted if ever nonzero in the freed nonpaged pool chains.
    //

    ULONGLONG Protection : 5;
    ULONGLONG Prototype : 1;        // MUST BE ZERO as per above comment.
    ULONGLONG Transition : 1;

    ULONGLONG filler1 : 20;
    ULONGLONG NextEntry : 32;
} MMPTE_LIST;

typedef struct _MMPTE_HIGHLOW
{
    ULONG LowPart;
    ULONG HighPart;
} MMPTE_HIGHLOW;

typedef struct _MMPTE_HARDWARE_LARGEPAGE
{
    ULONGLONG Valid : 1;
    ULONGLONG Write : 1;
    ULONGLONG Owner : 1;
    ULONGLONG WriteThrough : 1;
    ULONGLONG CacheDisable : 1;
    ULONGLONG Accessed : 1;
    ULONGLONG Dirty : 1;
    ULONGLONG LargePage : 1;
    ULONGLONG Global : 1;
    ULONGLONG CopyOnWrite : 1; // software field
    ULONGLONG Prototype : 1;   // software field
    ULONGLONG reserved0 : 1;   // software field
    ULONGLONG PAT : 1;
    ULONGLONG reserved1 : 8;   // software field
    ULONGLONG PageFrameNumber : 19;
    ULONGLONG reserved2 : 24;   // software field
} MMPTE_HARDWARE_LARGEPAGE;

//
// A Page Table Entry on AMD64 has the following definition.
// Note the MP version is to avoid stalls when flushing TBs across processors.
//

//
// Uniprocessor version.
//

typedef struct _MMPTE_HARDWARE
{
    ULONGLONG Valid : 1;
#if defined(NT_UP)
    ULONGLONG Write : 1;        // UP version
#else
    ULONGLONG Writable : 1;        // changed for MP version
#endif
    ULONGLONG Owner : 1;
    ULONGLONG WriteThrough : 1;
    ULONGLONG CacheDisable : 1;
    ULONGLONG Accessed : 1;
    ULONGLONG Dirty : 1;
    ULONGLONG LargePage : 1;
    ULONGLONG Global : 1;
    ULONGLONG CopyOnWrite : 1; // software field
    ULONGLONG Prototype : 1;   // software field
#if defined(NT_UP)
    ULONGLONG reserved0 : 1;  // software field
#else
    ULONGLONG Write : 1;       // software field - MP change
#endif
    ULONGLONG PageFrameNumber : 28;
    ULONG64 reserved1 : 24 - (_HARDWARE_PTE_WORKING_SET_BITS + 1);
    ULONGLONG SoftwareWsIndex : _HARDWARE_PTE_WORKING_SET_BITS;
    ULONG64 NoExecute : 1;
} MMPTE_HARDWARE;

#if defined(NT_UP)
#define HARDWARE_PTE_DIRTY_MASK     0x40
#else
#define HARDWARE_PTE_DIRTY_MASK     0x42
#endif

#define MI_GET_PAGE_FRAME_FROM_PTE(PTE) ((PTE)->u.Hard.PageFrameNumber)
#define MI_GET_PAGE_FRAME_FROM_TRANSITION_PTE(PTE) ((PTE)->u.Trans.PageFrameNumber)
#define MI_GET_PROTECTION_FROM_SOFT_PTE(PTE) ((ULONG)(PTE)->u.Soft.Protection)
#define MI_GET_PROTECTION_FROM_TRANSITION_PTE(PTE) ((ULONG)(PTE)->u.Trans.Protection)

#define PAGE_DIRECTORY0_MASK (MM_VA_MAPPED_BY_PXE - 1)
#define PAGE_DIRECTORY1_MASK (MM_VA_MAPPED_BY_PPE - 1)
#define PAGE_DIRECTORY2_MASK (MM_VA_MAPPED_BY_PDE - 1)

#define PTE_SHIFT 3

#define MM_MINIMUM_VA_FOR_LARGE_PAGE MM_VA_MAPPED_BY_PDE

//
// The number of bits in a virtual address.
//

#define VIRTUAL_ADDRESS_BITS 48
#define VIRTUAL_ADDRESS_MASK ((((ULONG_PTR)1) << VIRTUAL_ADDRESS_BITS) - 1)

typedef struct _MMPTE
{
    union
    {
        ULONG_PTR Long;
        MMPTE_HARDWARE Hard;
        MMPTE_HARDWARE_LARGEPAGE HardLarge;
        HARDWARE_PTE Flush;
        MMPTE_PROTOTYPE Proto;
        MMPTE_SOFTWARE Soft;
        MMPTE_TRANSITION Trans;
        MMPTE_SUBSECTION Subsect;
        MMPTE_LIST List;
    } u;
} MMPTE;

#define MiGetPxeAddress(va)   ((MMPTE *)PXE_BASE + MiGetPxeOffset(va))
#define MiGetPpeAddress(va)   \
    ((MMPTE *)(((((ULONG_PTR)(va) & VIRTUAL_ADDRESS_MASK) >> PPI_SHIFT) << PTE_SHIFT) + PPE_BASE))
#define MiGetPdeAddress(va)  \
    ((MMPTE *)(((((ULONG_PTR)(va) & VIRTUAL_ADDRESS_MASK) >> PDI_SHIFT) << PTE_SHIFT) + PDE_BASE))
#define MiGetPteAddress(va) \
    ((MMPTE *)(((((ULONG_PTR)(va) & VIRTUAL_ADDRESS_MASK) >> PTI_SHIFT) << PTE_SHIFT) + PTE_BASE))

#pragma warning(pop)

#define IsPteValid(pte) (pte->u.Hard.Valid)
#define IsPteDirty(pte) (pte->u.Hard.Dirty)
#define IsPteLarge(pte) (IsPteValid(pte) && pte->u.Hard.LargePage)

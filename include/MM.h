#ifndef MM_H
#define MM_H

#include <stdint.h>
#include "MemHier.h"
#include "WB.h"
#include "TLB.h"

#define MM_NO_FRAME  ((uint32_t)-1)

/* What one fault did, as OR-able flags. NOT MUTUALLY EXCLUSIVE. Help in error handling
 * and in statistics. 
 */
typedef enum {
    MM_OK          = 0,
    MM_DISK_READ   = 1 << 0,   /* Page read from disk          */
    MM_EVICTED     = 1 << 1,   /* a frame evicted from main_memory    */
    MM_WROTE_BACK  = 1 << 2,   /* that frame was dirty and was flushed    */
    MM_OOM         = 1 << 3    /* victim couldn't be chosen    */
} MMResult;

/* ---- protection flags ---- */
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* Occupation type/status */
typedef enum {
    FRAME_FREE = 0,     /* nobody owns the frame */
    FRAME_DATA,         /* holds a page of process */
    FRAME_PGTBL         /* holds the page table of a process */
} FrameKind;


typedef struct {
    uint32_t present    : 1;            /* same as valid/invalid */
    uint32_t frame      : FRAME_BITS;   /* frame number */
    uint32_t referenced : 1;           
    uint32_t dirty      : 1;           
    uint32_t prot       : 3;            
} PTE;                                  /* 21 bits, padded to 4 B */

typedef struct {
    PTE entries[PAGES_PER_PROC];        /* 256 */
} PageTable;

/* Frame Descriptor */
typedef struct {
    uint8_t  kind;         
    uint16_t pid;           /* pid of process owning frame */
    uint8_t  vpn;           /* vpn mapped to pfn */
    uint8_t  aging;         /* 8-bit shift register. */
} FrameDesc;

typedef struct {
    uint16_t   pid;
    PageTable *pt;              
    uint32_t   pt_frame;       
    uint32_t   frames_held;     
    uint32_t   lower_limit;     
    uint32_t   upper_limit;     
} Process;

typedef struct {
    FrameDesc frames[NUM_FRAMES];
    uint32_t  free_list[NUM_FRAMES];    /* free frame list*/
    uint32_t  free_count;
    Process  *procs;                    /* OS process table */
    uint16_t  num_procs;                /* For global replacement */
} MM;


int  mm_init(MM *mm, Process *procs, uint16_t num_procs);

/* Releases the frame table. */
void mm_destroy(MM *mm);

/* allocate a free frame to hold vpn for pid.If frame unavailable return -1 */
int  mm_alloc_frame(MM *mm, uint16_t pid, uint8_t vpn, FrameKind kind);

/* return globally coldest victim */
int  mm_select_victim(const MM *mm);

/* Samples every Accessed bit into its frame's aging register, clears the bit,
 * and invalidates that TLB entry so the next access is forced to walk. */
void mm_age_tick(MM *mm, TLB *tlb);

/* Initialize the process->prepage 2 pages -> also bring page table to memory
 * Returns 0, or -1 if memory cannot seat it. */
int  mm_create_process(MM *mm, Process *proc, uint16_t pid,
                       uint32_t lower_limit, uint32_t upper_limit);

/* handle page fault-> check for free frame -> choose victim*/
MMResult mm_handle_fault(MM *mm, Process *proc, uint8_t vpn,
                         WriteBuffer *wb, uint32_t *out_frame);

/* Track read from memory */
void mm_read_block(MM *mm, uint32_t pa);

/* Track write to memory */
void mm_write(MM *mm, uint32_t pa);

void mm_dump(const MM *mm);

#endif /* MM_H */

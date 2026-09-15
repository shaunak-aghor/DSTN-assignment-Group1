/*
MainMemory logic
Handles m/m page fault,choosing victim for eviction based on lfu with aging
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MM.h"


typedef char pt_fits_one_frame[(sizeof(PageTable) == PAGE_SIZE) ? 1 : -1];


typedef char vpn_fits_uint8[(PAGES_PER_PROC == 256 && VPN_BITS == 8) ? 1 : -1];

#define NO_FRAME    ((uint32_t)-1)
#define ANY_PID     (-1)



static int frame_pop(MM *mm)
{
    if (mm->free_count == 0)
        return -1;
    return (int)mm->free_list[--mm->free_count];
}

static void frame_push(MM *mm, uint32_t f)
{
    memset(&mm->frames[f], 0, sizeof(FrameDesc));   
    mm->free_list[mm->free_count++] = f;
}

/* Return the process object for process identified pid*/

static Process *proc_of(MM *mm, uint16_t pid)
{
    uint16_t i;

    if (!mm->procs)
        return NULL;

   /* Search for process in list of processes based on pid*/
    for (i = 0; i < mm->num_procs; i++)
        if (mm->procs[i].pt && mm->procs[i].pid == pid)
            return &mm->procs[i];

    return NULL;                
}


/* initialize the main memory*/
int mm_init(MM *mm, Process *procs, uint16_t num_procs)
{
    uint32_t i;

    if (!mm)
        return -1;

    memset(mm, 0, sizeof(*mm));
    mm->procs     = procs;
    mm->num_procs = num_procs;

  
    if (procs && num_procs)
        memset(procs, 0, (size_t)num_procs * sizeof(Process));

   
    for (i = NUM_FRAMES; i-- > 0; )
        frame_push(mm, i);

    return 0;
}

void mm_destroy(MM *mm)
{
    if (!mm) return;
    memset(mm->frames, 0, sizeof(mm->frames));
    mm->free_count = 0;
}


/*
Frame allocation
*/

int mm_alloc_frame(MM *mm, uint16_t pid, uint8_t vpn, FrameKind kind)
{
    int f;
    FrameDesc *fd;

    if (kind == FRAME_FREE)
        return -1;

    f = frame_pop(mm);
    if (f < 0)
        return -1;                     

    fd = &mm->frames[f];
    fd->kind       = (uint8_t)kind;
    fd->pid        = pid;
    fd->vpn        = vpn;
    fd->aging      = 0x80;              

    return f;
}



static int pick_victim(const MM *mm, int pid_filter)
{
    uint32_t f;
    uint32_t best     = NO_FRAME;
    uint8_t  best_age = 0;

    for (f = 0; f < NUM_FRAMES; f++) {
        const FrameDesc *fd = &mm->frames[f];

        if (fd->kind != FRAME_DATA)                 /* free, or a page table */
            continue;

        if (pid_filter != ANY_PID) {
            if (fd->pid != (uint16_t)pid_filter)
                continue;
        } else {
            const Process *ow = proc_of((MM *)mm, fd->pid);
            if (ow && ow->frames_held <= ow->lower_limit)
                continue;                           /* at its floor */
        }

        /* Strictly less, so an equal aging keeps the earlier (lower) frame --
         * deterministic tie-breaking with no extra state. */
        if (best == NO_FRAME || fd->aging < best_age) {
            best     = f;
            best_age = fd->aging;
        }
    }

    return (best == NO_FRAME) ? -1 : (int)best;
}

int mm_select_victim(const MM *mm)
{
    return pick_victim(mm, ANY_PID);
}



void mm_age_tick(MM *mm, TLB *tlb)
{
    uint16_t i;
    uint32_t v;

    if (!mm || !mm->procs)
        return;

   
    for (i = 0; i < mm->num_procs; i++) {
        Process *proc = &mm->procs[i];

        if (!proc->pt)
            continue;

        for (v = 0; v < PAGES_PER_PROC; v++) {
            PTE       *pte = &proc->pt->entries[v];
            FrameDesc *fd;

            if (!pte->present || pte->frame >= NUM_FRAMES)
                continue;

            fd = &mm->frames[pte->frame];
            fd->aging = (uint8_t)((fd->aging >> 1) |
                                  (pte->referenced ? 0x80u : 0x00u));

            if (pte->referenced) {
                pte->referenced = 0;

              
                if (tlb)
                    tlb_invalidate_entry(tlb, proc->pid, v);
            }
        }
    }
}



static MMResult evict_frame(MM *mm, uint32_t f, WriteBuffer *wb,
                            uint32_t *out_frame)
{
    FrameDesc *fd = &mm->frames[f];
    uint16_t   pid;
    uint8_t    vpn;
    Process   *ow;
    WBEntry    e;
    MMResult   r = MM_OK;
    int        guard = WB_ENTRIES + 1;  

    while (wb && wb_probe_frame(wb, f) >= 0 && guard-- > 0) {
        uint32_t pa, df;

        if (!wb_drain_head(wb, &e))
            break;

        pa = WB_TO_PA(e.block_addr);
        df = (uint32_t)PA_FRAME(pa);


        if (df < NUM_FRAMES && mm->frames[df].kind == FRAME_DATA) {
            Process *dow = proc_of(mm, mm->frames[df].pid);
            if (dow && dow->pt) {
                PTE *dpte = &dow->pt->entries[mm->frames[df].vpn];
                if (dpte->present && dpte->frame == df)
                    dpte->dirty = 1;
            }
        }

    }

    pid = fd->pid;
    vpn = fd->vpn;
    ow  = proc_of(mm, pid);

    if (ow && ow->pt) {
        PTE *pte = &ow->pt->entries[vpn];

        if (pte->present && pte->frame == f) {
            if (pte->dirty)
                r |= MM_WROTE_BACK;         

            pte->present    = 0;
            pte->frame      = 0;
            pte->referenced = 0;
            pte->dirty      = 0;

            if (ow->frames_held)
                ow->frames_held--;
        }
    }

    if (out_frame)
        *out_frame = f;

    frame_push(mm, f);
    return r | MM_EVICTED;
}



 /*
 Page fault - CASES
  1. PAGE ALREADY PRESENT - SPURIOUS FAULT
  2. process already has its UPPER_LIMIT number of pages - it evicts one of its pages
  3. a free frame exits - that frame chosen for eviction
  4. if first three conditions were false -> evict frame of another process (global replacement)
 */
MMResult mm_handle_fault(MM *mm, Process *proc, uint8_t vpn,
                         WriteBuffer *wb, uint32_t *out_frame)
{
    MMResult r = MM_OK;
    PTE     *pte;
    int      f;

    if (out_frame)
        *out_frame = MM_NO_FRAME;

    if (!mm || !proc || !proc->pt)
        return MM_OOM;

    pte = &proc->pt->entries[vpn];

   /* NO PAGE FAULT-> page exists in m/m */
    if (pte->present)
        return MM_OK;                  


   /* local replacement -> evict one of own pages */
    if (proc->frames_held >= proc->upper_limit) {
        int v = pick_victim(mm, (int)proc->pid);
        if (v < 0)
            return MM_OOM;              
        r |= evict_frame(mm, (uint32_t)v, wb, out_frame);
    }

    /* 3. Search for free frame */
    f = mm_alloc_frame(mm, proc->pid, vpn, FRAME_DATA);

    /* no free frame available -> choose a global victim */
    if (f < 0) {
        int v = mm_select_victim(mm);
        if (v < 0)
            return MM_OOM;
        r |= evict_frame(mm, (uint32_t)v, wb, out_frame);

        f = mm_alloc_frame(mm, proc->pid, vpn, FRAME_DATA);
        if (f < 0)
            return MM_OOM;
    }

    r |= MM_DISK_READ;                 

    pte->present    = 1;
    pte->frame      = (uint32_t)f;
    pte->referenced = 1;                
    pte->dirty      = 0;                
    proc->frames_held++;

    return r;
}

/* ------------------------------------------------------------------------
 *  Processes
 * --------------------------------------------------------------------- */

int mm_create_process(MM *mm, Process *proc, uint16_t pid,
                      uint32_t lower_limit, uint32_t upper_limit)
{
    uint32_t v;
    int      f;

    if (!mm || !proc)
        return -1;

  
    if (mm->free_count < MIN_FRAMES_PER_PROC)
        return -1;

    memset(proc, 0, sizeof(*proc));
    proc->pid = pid;

    proc->lower_limit = (lower_limit < MIN_FRAMES_PER_PROC)
                      ? MIN_FRAMES_PER_PROC : lower_limit;
    proc->upper_limit = (upper_limit == 0 || upper_limit > PAGES_PER_PROC)
                      ? PAGES_PER_PROC : upper_limit;
    if (proc->upper_limit < proc->lower_limit)
        proc->upper_limit = proc->lower_limit;      
    proc->pt = (PageTable *)calloc(1, sizeof(PageTable));
    if (!proc->pt)
        return -1;

        /* Allocate one frame for to hold page table*/
    f = mm_alloc_frame(mm, pid, 0, FRAME_PGTBL);
    if (f < 0) {
        free(proc->pt);
        proc->pt = NULL;
        return -1;
    }

    proc->pt_frame    = (uint32_t)f;
    proc->frames_held = 1;              

  
    for (v = 0; v < PAGES_PER_PROC; v++)
        proc->pt->entries[v].prot = PROT_READ | PROT_WRITE | PROT_EXEC;

    /* prepage first two pages */
    for (v = 0; v < 2; v++) {
        if (mm_handle_fault(mm, proc, (uint8_t)v, NULL, NULL) & MM_OOM) {
            free(proc->pt);
            proc->pt = NULL;
            return -1;
        }
    }

    return 0;
}


void mm_read_block(MM *mm, uint32_t pa)
{
    (void)pa;
    if (!mm) return;
}

void mm_write(MM *mm, uint32_t pa)
{
    (void)pa;
    if (!mm) return;
}


void mm_dump(const MM *mm)
{
    uint32_t f, data = 0, pgtbl = 0;

    if (!mm) return;

    for (f = 0; f < NUM_FRAMES; f++) {
        if (mm->frames[f].kind == FRAME_DATA)  data++;
        else if (mm->frames[f].kind == FRAME_PGTBL) pgtbl++;
    }

    printf("\n--- Main memory (%u frames of %d B, global LFU+aging) ---\n",
           (unsigned)NUM_FRAMES, PAGE_SIZE);
    printf("  free %u | data %u | page-table %u\n",
           (unsigned)mm->free_count, (unsigned)data, (unsigned)pgtbl);
}
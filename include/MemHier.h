#ifndef MEMHIER_CONFIG_H
#define MEMHIER_CONFIG_H

#include <stdint.h>

/*
 * Shared configuration for the memory hierarchy.
 * Every component header includes this.
 * Nothing here depends on any component.
 * Virtual address  : 18 bits = VPN 8 + offset 10
 * Physical address : 25 bits = frame 15 + offset 10
 * Page / frame size: 1 KB   -> 32768 frames in 32 MB
 * Cache block      : 16 B
 * All is done here.
*/

#define BLOCK_SIZE          16
#define STORE_WIDTH         4
#define PAGE_SIZE           1024

/*Virtual addressing*/
#define VA_BITS             18
#define VPN_BITS            8
#define PAGE_OFFSET_BITS    10
#define PAGES_PER_PROC      256         /*2^VPN_BITS = 2^8 KB*/

/*Physical addressing*/
#define PA_BITS             25
#define FRAME_BITS          15
#define NUM_FRAMES          32768       /*2^FRAME_BITS*/
#define MM_SIZE             33554432UL  /*32 MB, unsigned long prevents overflow issues*/

/*L1: 4 KB, 16 B block, 4-way, LRU, write buffer*/
#define L1_SETS             64
#define L1_WAYS             4
#define L1_TAG_BITS         15
#define L1_INDEX_BITS       6
#define L1_OFFSET_BITS      4
#define L1_LRU_BITS         2           /*log2(L1_WAYS), per line*/

/*L2: 32 KB, 16 B block, 8-way, FIFO, write-through*/
#define L2_SETS             256
#define L2_WAYS             8
#define L2_TAG_BITS         13
#define L2_INDEX_BITS       8
#define L2_OFFSET_BITS      4
#define L2_FIFO_BITS        3           /* log2(L2_WAYS), per SET */

/*Write buffer and TLB*/
#define WB_ENTRIES          4
#define TLB_ENTRIES         32
#define PID_BITS            14

/*
 *Minimum resident frames per process.
 *2 pre-paged pages + 1 frame holding its page table.
 *everything else fits in the 1 extra frame(982B), change if needed.
 */
#define MIN_FRAMES_PER_PROC 3

/*Address decomposition*/

/*Masking macro, returns a mask of n bits, e.g. MASK(3) = 0b111*/
#define MASK(n)             ((1UL << (n)) - 1UL)

/*Virtual address decomposition, breaks it into VPN and offset*/
#define VA_VPN(va)          (((va) >> PAGE_OFFSET_BITS) & MASK(VPN_BITS))
#define VA_OFFSET(va)       ((va) & MASK(PAGE_OFFSET_BITS))

/*Physical address decomposition, breaks it into frame and offset*/
#define PA_FRAME(pa)        (((pa) >> PAGE_OFFSET_BITS) & MASK(FRAME_BITS))
#define PA_OFFSET(pa)       ((pa) & MASK(PAGE_OFFSET_BITS))

/*L1 address decomposition, bit shifts pa to get the info in L1 when needed*/
#define L1_INDEX(pa)        (((pa) >> L1_OFFSET_BITS) & MASK(L1_INDEX_BITS))
#define L1_TAG(pa)          (((pa) >> (L1_OFFSET_BITS + L1_INDEX_BITS)) & MASK(L1_TAG_BITS))
#define L1_BLK_OFFSET(pa)   ((pa) & MASK(L1_OFFSET_BITS))

/*L2 address decomposition, bit shifts pa to get the info in L2 when needed*/
#define L2_INDEX(pa)        (((pa) >> L2_OFFSET_BITS) & MASK(L2_INDEX_BITS))
#define L2_TAG(pa)          (((pa) >> (L2_OFFSET_BITS + L2_INDEX_BITS)) & MASK(L2_TAG_BITS))
#define L2_BLK_OFFSET(pa)   ((pa) & MASK(L2_OFFSET_BITS))

/*Base address of the block containing pa*/
#define BLOCK_BASE(pa)      ((pa) & ~MASK(L1_OFFSET_BITS))

/*Rebuild a physical address from an L1 tag and set index*/
#define L1_MAKE_PA(tag, idx) \
    ((((uint32_t)(tag)) << (L1_OFFSET_BITS + L1_INDEX_BITS)) | \
     (((uint32_t)(idx)) << L1_OFFSET_BITS))

/*Rebuild a physical address from an L2 tag and set index*/
#define L2_MAKE_PA(tag, idx) \
    ((((uint32_t)(tag)) << (L2_OFFSET_BITS + L2_INDEX_BITS)) | \
     (((uint32_t)(idx)) << L2_OFFSET_BITS))

#endif
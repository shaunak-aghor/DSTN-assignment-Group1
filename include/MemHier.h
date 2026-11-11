#ifndef MEMHIER_CONFIG_H
#define MEMHIER_CONFIG_H

#include <stdint.h>

/*
 * Sizes and bit widths for the whole simulator. Every header includes this.
 *
 * Virtual address  : 18 bits = VPN 8 + offset 10
 * Physical address : 25 bits = frame 15 + offset 10
 * Page / frame     : 1 KB  -> 32768 frames in 32 MB
 * Cache block      : 16 B
 */

#define BLOCK_SIZE          16
#define PAGE_SIZE           1024

/* Virtual address */
#define VA_BITS             18
#define VPN_BITS            8
#define PAGE_OFFSET_BITS    10
#define PAGES_PER_PROC      256         /* 2^VPN_BITS */

/* Physical address */
#define PA_BITS             25
#define FRAME_BITS          15
#define NUM_FRAMES          32768       /* 2^FRAME_BITS */

/* L1: 4 KB, 16 B block, 4-way, LRU */
#define L1_SETS             64
#define L1_WAYS             4
#define L1_TAG_BITS         15
#define L1_INDEX_BITS       6
#define L1_OFFSET_BITS      4

/* L2: 32 KB, 16 B block, 8-way, FIFO */
#define L2_SETS             256
#define L2_WAYS             8
#define L2_TAG_BITS         13
#define L2_INDEX_BITS       8
#define L2_OFFSET_BITS      4

/* Write buffer and TLB */
#define WB_ENTRIES          4
#define TLB_ENTRIES         32
#define PID_BITS            14

/* Frames a process always keeps: 1 page table + 2 pre-paged pages */
#define MIN_FRAMES_PER_PROC 3

/* MASK(n) = n ones, e.g. MASK(3) = 0b111 */
#define MASK(n)             ((1UL << (n)) - 1UL)

/* Virtual address -> VPN | offset */
#define VA_VPN(va)          (((va) >> PAGE_OFFSET_BITS) & MASK(VPN_BITS))
#define VA_OFFSET(va)       ((va) & MASK(PAGE_OFFSET_BITS))

/* Physical address -> frame number */
#define PA_FRAME(pa)        (((pa) >> PAGE_OFFSET_BITS) & MASK(FRAME_BITS))

/* Physical address -> L1 tag | index | offset */
#define L1_INDEX(pa)        (((pa) >> L1_OFFSET_BITS) & MASK(L1_INDEX_BITS))
#define L1_TAG(pa)          (((pa) >> (L1_OFFSET_BITS + L1_INDEX_BITS)) & MASK(L1_TAG_BITS))

/* Physical address -> L2 tag | index | offset */
#define L2_INDEX(pa)        (((pa) >> L2_OFFSET_BITS) & MASK(L2_INDEX_BITS))
#define L2_TAG(pa)          (((pa) >> (L2_OFFSET_BITS + L2_INDEX_BITS)) & MASK(L2_TAG_BITS))

/* Rebuild a block's physical address from its tag and set index */
#define L1_MAKE_PA(tag, idx) \
    ((((uint32_t)(tag)) << (L1_OFFSET_BITS + L1_INDEX_BITS)) | \
     (((uint32_t)(idx)) << L1_OFFSET_BITS))

#define L2_MAKE_PA(tag, idx) \
    ((((uint32_t)(tag)) << (L2_OFFSET_BITS + L2_INDEX_BITS)) | \
     (((uint32_t)(idx)) << L2_OFFSET_BITS))

#endif

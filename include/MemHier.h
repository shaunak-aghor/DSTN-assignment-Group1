#ifndef MEMHIER_CONFIG_H
#define MEMHIER_CONFIG_H

#include <stdint.h>

/*
 * Sizes and bit widths for the whole simulator. Every header includes this.
 *
 * Virtual address  : 32 bits = VPN 22 (outer 6 | middle 8 | inner 8) + offset 10
 * Physical address : 27 bits = frame 17 + offset 10  (can address 128 MB)
 * Installed memory : 32 MB = 32768 frames of 1 KB    (only needs 25 bits)
 * Cache block      : 16 B
 *
 * The PA is 2 bits wider than the installed memory so memory can grow
 * later without changing any address format, cache tag or page table.
 */

#define BLOCK_SIZE          16
#define PAGE_SIZE           1024

/* Virtual address and the 3-level page table */
#define VA_BITS             32
#define PAGE_OFFSET_BITS    10
#define VPN_BITS            22          /* VA_BITS - PAGE_OFFSET_BITS */
#define OUTER_BITS          6           /* outer table:  64 entries  */
#define MIDDLE_BITS         8           /* middle table: 256 entries */
#define INNER_BITS          8           /* inner table:  256 entries */
#define PT_ENTRIES          256         /* PTEs in one 1 KB frame (4 B each) */

/* Physical address */
#define PA_BITS             27
#define FRAME_BITS          17          /* PA_BITS - PAGE_OFFSET_BITS */
#define NUM_FRAMES          32768       /* frames actually installed (32 MB) */

/* L1: 4 KB, 16 B block, 4-way, LRU */
#define L1_SETS             64
#define L1_WAYS             4
#define L1_INDEX_BITS       6
#define L1_OFFSET_BITS      4
#define L1_TAG_BITS         (PA_BITS - L1_INDEX_BITS - L1_OFFSET_BITS)   /* 17 */

/* L2: 32 KB, 16 B block, 8-way, FIFO */
#define L2_SETS             256
#define L2_WAYS             8
#define L2_INDEX_BITS       8
#define L2_OFFSET_BITS      4
#define L2_TAG_BITS         (PA_BITS - L2_INDEX_BITS - L2_OFFSET_BITS)   /* 15 */

/* Write buffer and TLB */
#define WB_ENTRIES          4
#define TLB_ENTRIES         32
#define PID_BITS            14

/* A new process needs 3 table frames (one per level) + 2 pre-paged pages */
#define MIN_FRAMES_PER_PROC 5
#define MIN_PAGES_PER_PROC  2

/* MASK(n) = n ones, e.g. MASK(3) = 0b111 */
#define MASK(n)             ((1UL << (n)) - 1UL)

/* Virtual address -> VPN | offset */
#define VA_VPN(va)          (((va) >> PAGE_OFFSET_BITS) & MASK(VPN_BITS))
#define VA_OFFSET(va)       ((va) & MASK(PAGE_OFFSET_BITS))

/* VPN -> index into each level of the page table */
#define VPN_OUTER(vpn)      (((vpn) >> (MIDDLE_BITS + INNER_BITS)) & MASK(OUTER_BITS))
#define VPN_MIDDLE(vpn)     (((vpn) >> INNER_BITS) & MASK(MIDDLE_BITS))
#define VPN_INNER(vpn)      ((vpn) & MASK(INNER_BITS))

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

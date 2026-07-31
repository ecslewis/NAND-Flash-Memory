#ifndef FTL_H
#define FTL_H

#include <stddef.h>
#include <stdint.h>

#include "nand_flash.h"

/* -----------------------------------------------------------------------
 * Flash Translation Layer (FTL)
 *
 * Sits on top of NandFlash and exposes a simple logical page interface:
 * write(lpn, data) / read(lpn) / trim(lpn), backed by:
 *
 *   - A logical-to-physical (L2P) mapping table.
 *   - Out-of-place updates: a write to an already-mapped logical page
 *     doesn't overwrite in place (NAND can't do that); instead it writes
 *     to a fresh page in the current "active" block and marks the old
 *     physical page STALE (garbage).
 *   - A free-block pool of fully erased blocks to write into.
 *   - Garbage collection: when a new active block is needed and the free
 *     pool is empty, the block with the most stale (garbage) pages is
 *     chosen as a GC victim. Any still-valid pages in it are relocated
 *     (rewritten) into the active block, then the victim is erased and
 *     returned to the free pool.
 *   - Static wear leveling on block allocation: when picking a new
 *     active block from the free pool, the least-erased free block is
 *     chosen, so erases get spread out over time.
 *
 * This is deliberately a simple greedy-GC FTL for learning purposes, not
 * a production design: no crash-consistency/journal, no cost-benefit GC
 * weighting, no bad-block handling.
 *
 * The underlying NandFlash should be freshly created (all blocks erased)
 * before handing it to ftl_create() -- the FTL assumes every block starts
 * out free. The FTL does not take ownership of the NandFlash; the caller
 * is still responsible for calling nand_destroy() on it.
 * ---------------------------------------------------------------------*/

typedef enum
{
    FTL_PAGE_FREE = 0, /* physical page is erased and unused */
    FTL_PAGE_VALID,    /* physical page holds the current data for some lpn */
    FTL_PAGE_STALE     /* physical page holds superseded data (garbage) */
} FtlPageState;

typedef enum
{
    FTL_OK = 0,
    FTL_ERR_RANGE,    /* lpn out of range */
    FTL_ERR_SIZE,     /* data length != page size */
    FTL_ERR_UNMAPPED, /* read/trim of an lpn that isn't currently mapped */
    FTL_ERR_NO_SPACE, /* no free blocks, and GC couldn't reclaim any */
    FTL_ERR_ALLOC,    /* memory allocation failure */
    FTL_ERR_NAND      /* an underlying NandFlash call unexpectedly failed */
} FtlStatus;

typedef struct
{
    NandFlash *nand; /* not owned; caller creates/destroys the NandFlash */
    size_t numLogicalPages;

    /* L2P table: physical location of each logical page, or -1 if unmapped */
    int64_t *l2pBlock;
    int64_t *l2pPage;

    /* Per-physical-page state and reverse map, indexed [block][page] */
    FtlPageState **pageState;
    int64_t **physToLpn; /* which lpn a VALID page holds, else -1 */

    /* Per-block accounting */
    size_t *validCount; /* # of VALID pages in this block */
    size_t *staleCount; /* # of STALE pages in this block */

    /* Free block pool (indices of blocks that are fully erased) */
    size_t *freeBlocks;
    size_t freeBlockCount;

    int64_t activeBlock;   /* block currently being written into, -1 if none */
    size_t activeNextPage; /* next page index to program in activeBlock */

    /* Stats */
    uint64_t hostWrites;     /* writes requested via ftl_write() */
    uint64_t physicalWrites; /* total nand_program_page calls (host + GC relocations) */
    uint64_t gcRuns;
    uint64_t pagesRelocated;
} Ftl;

/* Creates an FTL instance over an existing, freshly-erased NandFlash.
 * numLogicalPages should be less than the NAND's total physical page
 * count to leave over-provisioned spare blocks for GC to work with --
 * see ftl_recommended_logical_pages(). Returns NULL on allocation
 * failure or if numLogicalPages is 0 or >= total physical pages. */
Ftl *ftl_create(NandFlash *nand, size_t numLogicalPages);

/* Frees all memory associated with the FTL instance. Does NOT touch the
 * underlying NandFlash -- that's still owned by the caller. */
void ftl_destroy(Ftl *ftl);

/* Suggests a logical page count that leaves `reserveBlocks` worth of
 * blocks as spare over-provisioning for GC to have room to work with. */
size_t ftl_recommended_logical_pages(const NandFlash *nand, size_t reserveBlocks);

/* Writes `data` (must be exactly the NAND's page size) to logical page
 * `lpn`. If `lpn` was already written, the old physical copy is marked
 * stale and reclaimed later by GC -- this is an out-of-place update.
 * May trigger an automatic GC pass if the free pool is empty. */
FtlStatus ftl_write(Ftl *ftl, size_t lpn, const uint8_t *data, size_t dataLen);

/* Reads the current data for logical page `lpn`. Fails with
 * FTL_ERR_UNMAPPED if it has never been written (or was trimmed). */
FtlStatus ftl_read(Ftl *ftl, size_t lpn, const uint8_t **outData);

/* Marks `lpn` as deleted (like a TRIM/discard command): its physical
 * page becomes stale immediately, without writing new data. */
FtlStatus ftl_trim(Ftl *ftl, size_t lpn);

/* Forces a single garbage-collection pass, reclaiming one victim block
 * if any block currently has stale pages worth reclaiming. GC also runs
 * automatically inside ftl_write() when needed; this lets you trigger
 * it manually to watch it work. Returns FTL_ERR_NO_SPACE if no block
 * currently has any reclaimable garbage. */
FtlStatus ftl_force_gc(Ftl *ftl);

/* Prints a status table: free/active blocks, per-block valid/stale/free
 * page counts, and write-amplification / GC stats. */
void ftl_print_status(const Ftl *ftl);

const char *ftl_status_str(FtlStatus status);
const char *ftl_last_error(void);

#endif /* FTL_H */
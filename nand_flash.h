#ifndef NAND_FLASH_H
#define NAND_FLASH_H

#include <stddef.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * NAND Flash Simulator (C)
 *
 * A simplified simulation of NAND flash memory geometry and the core
 * physical rules that make NAND different from RAM or a regular file:
 *
 *   1. Memory is organized as Blocks -> Pages -> Bytes.
 *   2. Erasing is only possible at the BLOCK granularity. Erasing sets
 *      every bit in the block to 1 (0xFF per byte).
 *   3. Programming (writing) happens at PAGE granularity, and can only
 *      flip bits from 1 -> 0. That's why a page must be freshly erased
 *      before it can be programmed again.
 *   4. Within a block, pages must be programmed sequentially (page 0,
 *      then 1, then 2, ...). This mirrors real NAND datasheets, which
 *      forbid programming pages out of order within a block.
 *   5. Reading is unrestricted -- any page can be read at any time,
 *      returning whatever bit pattern is currently stored (0xFF bytes
 *      for pages that have never been programmed since the last erase).
 * ---------------------------------------------------------------------*/

typedef enum
{
    PAGE_ERASED,    /* all bits 1 (0xFF), ready to be programmed */
    PAGE_PROGRAMMED /* has been written since the last erase */
} PageState;

typedef enum
{
    NAND_OK = 0,
    NAND_ERR_RANGE,              /* block or page index out of range */
    NAND_ERR_SIZE,               /* data length != page size */
    NAND_ERR_ALREADY_PROGRAMMED, /* page must be erased before programming */
    NAND_ERR_OUT_OF_ORDER,       /* pages must be programmed sequentially */
    NAND_ERR_ALLOC               /* memory allocation failure */
} NandStatus;

typedef struct
{
    uint64_t eraseCount;
} BlockStats;

typedef struct
{
    size_t numBlocks;
    size_t pagesPerBlock;
    size_t pageSizeBytes;

    uint8_t ***storage;      /* storage[block][page][byte] */
    PageState **pageStates;  /* pageStates[block][page] */
    size_t *programmedCount; /* pages programmed so far, per block */
    BlockStats *stats;       /* per-block stats (erase count, etc.) */
} NandFlash;

/* Creates a NandFlash instance. Returns NULL on allocation failure or
 * invalid (zero) geometry. Caller must call nand_destroy() when done. */
NandFlash *nand_create(size_t numBlocks, size_t pagesPerBlock, size_t pageSizeBytes);

/* Frees all memory associated with a NandFlash instance. */
void nand_destroy(NandFlash *flash);

/* Erases an entire block: resets all its pages to 0xFF and marks them
 * PAGE_ERASED. Increments the block's erase counter. */
NandStatus nand_erase_block(NandFlash *flash, size_t blockIdx);

/* Programs a single page with `data` (must be exactly pageSizeBytes long).
 * Fails with:
 *   NAND_ERR_RANGE               - blockIdx/pageIdx out of range
 *   NAND_ERR_SIZE                - dataLen != pageSizeBytes
 *   NAND_ERR_ALREADY_PROGRAMMED  - page not currently erased
 *   NAND_ERR_OUT_OF_ORDER        - not the next sequential page in the block
 */
NandStatus nand_program_page(NandFlash *flash, size_t blockIdx, size_t pageIdx,
                             const uint8_t *data, size_t dataLen);

/* Reads a page's current contents. Always legal, regardless of state.
 * On success, *outData points at the internal buffer (pageSizeBytes long)
 * and NAND_OK is returned. On failure, *outData is untouched. */
NandStatus nand_read_page(const NandFlash *flash, size_t blockIdx, size_t pageIdx,
                          const uint8_t **outData);

PageState nand_get_page_state(const NandFlash *flash, size_t blockIdx, size_t pageIdx);
uint64_t nand_get_erase_count(const NandFlash *flash, size_t blockIdx);

/* Returns the index of the next page that must be programmed next in this
 * block (i.e. how many pages have been programmed since the last erase). */
size_t nand_next_programmable_page(const NandFlash *flash, size_t blockIdx);

/* Prints a compact status table (block, erase count, page states) to stdout. */
void nand_print_status(const NandFlash *flash);

/* Total capacity of the device in bytes (numBlocks * pagesPerBlock * pageSizeBytes). */
size_t nand_total_capacity_bytes(const NandFlash *flash);

/* Bytes currently "used" -- i.e. sum of pageSizeBytes over every page that
 * is in PAGE_PROGRAMMED state, across all blocks. */
size_t nand_used_bytes(const NandFlash *flash);

/* Short, fixed human-readable string for a status code. */
const char *nand_status_str(NandStatus status);

/* Detailed message (with block/page indices etc.) describing the most
 * recent error from this thread. Valid until the next nand_* call.
 * Not thread-safe (uses an internal static buffer) -- fine for this
 * single-threaded simulator. */
const char *nand_last_error(void);

#endif /* NAND_FLASH_H */
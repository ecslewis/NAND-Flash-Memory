#include "ftl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_ftlLastError[256] = "";

static void set_error(const char *msg)
{
    snprintf(g_ftlLastError, sizeof(g_ftlLastError), "%s", msg);
}

const char *ftl_last_error(void)
{
    return g_ftlLastError;
}

const char *ftl_status_str(FtlStatus status)
{
    switch (status)
    {
    case FTL_OK:
        return "OK";
    case FTL_ERR_RANGE:
        return "logical page out of range";
    case FTL_ERR_SIZE:
        return "data size mismatch";
    case FTL_ERR_UNMAPPED:
        return "logical page not mapped (never written / trimmed)";
    case FTL_ERR_NO_SPACE:
        return "device full (no space could be reclaimed)";
    case FTL_ERR_ALLOC:
        return "allocation failure";
    case FTL_ERR_NAND:
        return "underlying NAND operation failed";
    default:
        return "unknown error";
    }
}

size_t ftl_recommended_logical_pages(const NandFlash *nand, size_t reserveBlocks)
{
    if (reserveBlocks >= nand->numBlocks)
    {
        reserveBlocks = nand->numBlocks > 1 ? nand->numBlocks - 1 : 0;
    }
    size_t usableBlocks = nand->numBlocks - reserveBlocks;
    return usableBlocks * nand->pagesPerBlock;
}

static void ftl_free_all(Ftl *ftl)
{
    if (!ftl)
        return;

    if (ftl->pageState)
    {
        for (size_t b = 0; b < ftl->nand->numBlocks; ++b)
            free(ftl->pageState[b]);
        free(ftl->pageState);
    }
    if (ftl->physToLpn)
    {
        for (size_t b = 0; b < ftl->nand->numBlocks; ++b)
            free(ftl->physToLpn[b]);
        free(ftl->physToLpn);
    }
    free(ftl->l2pBlock);
    free(ftl->l2pPage);
    free(ftl->validCount);
    free(ftl->staleCount);
    free(ftl->freeBlocks);
    free(ftl);
}

Ftl *ftl_create(NandFlash *nand, size_t numLogicalPages)
{
    if (!nand)
    {
        set_error("ftl_create: nand is NULL");
        return NULL;
    }
    size_t totalPhysicalPages = nand->numBlocks * nand->pagesPerBlock;
    if (numLogicalPages == 0 || numLogicalPages >= totalPhysicalPages)
    {
        set_error("ftl_create: numLogicalPages must be > 0 and < total physical "
                  "pages (leave spare blocks for over-provisioning)");
        return NULL;
    }

    Ftl *ftl = calloc(1, sizeof(Ftl));
    if (!ftl)
    {
        set_error("ftl_create: allocation failure");
        return NULL;
    }

    ftl->nand = nand;
    ftl->numLogicalPages = numLogicalPages;
    ftl->activeBlock = -1;
    ftl->activeNextPage = 0;

    ftl->l2pBlock = malloc(numLogicalPages * sizeof(int64_t));
    ftl->l2pPage = malloc(numLogicalPages * sizeof(int64_t));
    ftl->validCount = calloc(nand->numBlocks, sizeof(size_t));
    ftl->staleCount = calloc(nand->numBlocks, sizeof(size_t));
    ftl->freeBlocks = malloc(nand->numBlocks * sizeof(size_t));
    ftl->pageState = calloc(nand->numBlocks, sizeof(FtlPageState *));
    ftl->physToLpn = calloc(nand->numBlocks, sizeof(int64_t *));

    if (!ftl->l2pBlock || !ftl->l2pPage || !ftl->validCount || !ftl->staleCount ||
        !ftl->freeBlocks || !ftl->pageState || !ftl->physToLpn)
    {
        set_error("ftl_create: allocation failure");
        ftl_free_all(ftl);
        return NULL;
    }

    for (size_t i = 0; i < numLogicalPages; ++i)
    {
        ftl->l2pBlock[i] = -1;
        ftl->l2pPage[i] = -1;
    }

    int allocOk = 1;
    for (size_t b = 0; b < nand->numBlocks; ++b)
    {
        ftl->pageState[b] = calloc(nand->pagesPerBlock, sizeof(FtlPageState));
        ftl->physToLpn[b] = malloc(nand->pagesPerBlock * sizeof(int64_t));
        if (!ftl->pageState[b] || !ftl->physToLpn[b])
        {
            allocOk = 0;
            break;
        }
        for (size_t p = 0; p < nand->pagesPerBlock; ++p)
        {
            ftl->pageState[b][p] = FTL_PAGE_FREE;
            ftl->physToLpn[b][p] = -1;
        }
    }
    if (!allocOk)
    {
        set_error("ftl_create: allocation failure");
        ftl_free_all(ftl);
        return NULL;
    }

    /* Every block starts fully erased -> the entire device goes into the
     * free pool (this is why the NandFlash must be freshly created). */
    for (size_t b = 0; b < nand->numBlocks; ++b)
    {
        ftl->freeBlocks[ftl->freeBlockCount++] = b;
    }

    return ftl;
}

void ftl_destroy(Ftl *ftl)
{
    ftl_free_all(ftl);
}

/* Pops the least-erased block out of the free pool (static wear leveling:
 * spreads erases across blocks instead of reusing the same ones). Returns
 * -1 if the free pool is empty. */
static int64_t pop_least_worn_free_block(Ftl *ftl)
{
    if (ftl->freeBlockCount == 0)
        return -1;

    size_t bestIdx = 0;
    uint64_t bestErase = nand_get_erase_count(ftl->nand, ftl->freeBlocks[0]);
    for (size_t i = 1; i < ftl->freeBlockCount; ++i)
    {
        uint64_t ec = nand_get_erase_count(ftl->nand, ftl->freeBlocks[i]);
        if (ec < bestErase)
        {
            bestErase = ec;
            bestIdx = i;
        }
    }

    size_t block = ftl->freeBlocks[bestIdx];
    ftl->freeBlocks[bestIdx] = ftl->freeBlocks[ftl->freeBlockCount - 1];
    ftl->freeBlockCount--;
    return (int64_t)block;
}

/* Picks the GC victim: the non-active, fully-closed block with the most
 * stale (garbage) pages. A block is "closed" once valid+stale accounts
 * for every page in it (i.e. it's not sitting free or still being
 * actively written). */
static int64_t select_gc_victim(const Ftl *ftl)
{
    int64_t victim = -1;
    size_t bestStale = 0;

    for (size_t b = 0; b < ftl->nand->numBlocks; ++b)
    {
        if ((int64_t)b == ftl->activeBlock)
            continue;
        size_t total = ftl->validCount[b] + ftl->staleCount[b];
        if (total != ftl->nand->pagesPerBlock)
            continue; /* not closed */
        if (ftl->staleCount[b] == 0)
            continue; /* nothing to reclaim */
        if (ftl->staleCount[b] > bestStale)
        {
            bestStale = ftl->staleCount[b];
            victim = (int64_t)b;
        }
    }
    return victim;
}

static FtlStatus run_gc_pass(Ftl *ftl)
{
    int64_t victim = select_gc_victim(ftl);
    if (victim < 0)
    {
        set_error("garbage collection: no block currently has reclaimable stale pages");
        return FTL_ERR_NO_SPACE;
    }

    NandFlash *nand = ftl->nand;
    size_t pagesPerBlock = nand->pagesPerBlock;

    for (size_t p = 0; p < pagesPerBlock; ++p)
    {
        if (ftl->pageState[victim][p] != FTL_PAGE_VALID)
            continue;
        int64_t lpn = ftl->physToLpn[victim][p];

        const uint8_t *data;
        if (nand_read_page(nand, (size_t)victim, p, &data) != NAND_OK)
        {
            set_error("garbage collection: failed to read valid page during relocation");
            return FTL_ERR_NAND;
        }
        /* Copy the bytes out before we potentially reuse buffers below --
         * nand_read_page returns a pointer into internal storage, and we
         * are about to call nand_program_page on a *different* page, which
         * is safe, but making an explicit copy keeps the intent clear. */
        uint8_t *copy = malloc(nand->pageSizeBytes);
        if (!copy)
        {
            set_error("garbage collection: allocation failure copying page");
            return FTL_ERR_ALLOC;
        }
        memcpy(copy, data, nand->pageSizeBytes);

        if (ftl->activeBlock < 0 || ftl->activeNextPage >= pagesPerBlock)
        {
            int64_t nextBlock = pop_least_worn_free_block(ftl);
            if (nextBlock < 0)
            {
                free(copy);
                set_error("garbage collection: ran out of free blocks mid-relocation "
                          "(device is essentially full)");
                return FTL_ERR_NO_SPACE;
            }
            ftl->activeBlock = nextBlock;
            ftl->activeNextPage = 0;
        }

        size_t destBlock = (size_t)ftl->activeBlock;
        size_t destPage = ftl->activeNextPage;

        NandStatus nst = nand_program_page(nand, destBlock, destPage, copy, nand->pageSizeBytes);
        free(copy);
        if (nst != NAND_OK)
        {
            set_error(nand_last_error());
            return FTL_ERR_NAND;
        }
        ftl->physicalWrites++;

        ftl->pageState[destBlock][destPage] = FTL_PAGE_VALID;
        ftl->physToLpn[destBlock][destPage] = lpn;
        ftl->validCount[destBlock]++;
        ftl->l2pBlock[lpn] = (int64_t)destBlock;
        ftl->l2pPage[lpn] = (int64_t)destPage;
        ftl->activeNextPage++;

        ftl->pageState[victim][p] = FTL_PAGE_STALE;
        ftl->physToLpn[victim][p] = -1;
        ftl->validCount[victim]--;
        ftl->staleCount[victim]++;

        ftl->pagesRelocated++;

        if (ftl->activeNextPage >= pagesPerBlock)
        {
            ftl->activeBlock = -1; /* filled up; next allocation picks a new one */
        }
    }

    if (nand_erase_block(nand, (size_t)victim) != NAND_OK)
    {
        set_error(nand_last_error());
        return FTL_ERR_NAND;
    }
    for (size_t p = 0; p < pagesPerBlock; ++p)
    {
        ftl->pageState[victim][p] = FTL_PAGE_FREE;
        ftl->physToLpn[victim][p] = -1;
    }
    ftl->validCount[victim] = 0;
    ftl->staleCount[victim] = 0;
    ftl->freeBlocks[ftl->freeBlockCount++] = (size_t)victim;

    ftl->gcRuns++;
    return FTL_OK;
}

FtlStatus ftl_force_gc(Ftl *ftl)
{
    return run_gc_pass(ftl);
}

FtlStatus ftl_write(Ftl *ftl, size_t lpn, const uint8_t *data, size_t dataLen)
{
    if (lpn >= ftl->numLogicalPages)
    {
        set_error("ftl_write: lpn out of range");
        return FTL_ERR_RANGE;
    }
    if (dataLen != ftl->nand->pageSizeBytes)
    {
        set_error("ftl_write: data size mismatch");
        return FTL_ERR_SIZE;
    }

    if (ftl->activeBlock < 0 || ftl->activeNextPage >= ftl->nand->pagesPerBlock)
    {
        int64_t block = pop_least_worn_free_block(ftl);
        if (block < 0)
        {
            FtlStatus gcSt = run_gc_pass(ftl);
            if (gcSt != FTL_OK)
                return gcSt;
            block = pop_least_worn_free_block(ftl);
            if (block < 0)
            {
                set_error("ftl_write: device full");
                return FTL_ERR_NO_SPACE;
            }
        }
        ftl->activeBlock = block;
        ftl->activeNextPage = 0;
    }

    size_t destBlock = (size_t)ftl->activeBlock;
    size_t destPage = ftl->activeNextPage;

    NandStatus nst = nand_program_page(ftl->nand, destBlock, destPage, data, dataLen);
    if (nst != NAND_OK)
    {
        set_error(nand_last_error());
        return FTL_ERR_NAND;
    }
    ftl->physicalWrites++;
    ftl->hostWrites++;

    if (ftl->l2pBlock[lpn] >= 0)
    {
        size_t oldB = (size_t)ftl->l2pBlock[lpn];
        size_t oldP = (size_t)ftl->l2pPage[lpn];
        ftl->pageState[oldB][oldP] = FTL_PAGE_STALE;
        ftl->physToLpn[oldB][oldP] = -1;
        ftl->validCount[oldB]--;
        ftl->staleCount[oldB]++;
    }

    ftl->pageState[destBlock][destPage] = FTL_PAGE_VALID;
    ftl->physToLpn[destBlock][destPage] = (int64_t)lpn;
    ftl->validCount[destBlock]++;
    ftl->l2pBlock[lpn] = (int64_t)destBlock;
    ftl->l2pPage[lpn] = (int64_t)destPage;
    ftl->activeNextPage++;

    if (ftl->activeNextPage >= ftl->nand->pagesPerBlock)
    {
        ftl->activeBlock = -1;
    }

    return FTL_OK;
}

FtlStatus ftl_read(Ftl *ftl, size_t lpn, const uint8_t **outData)
{
    if (lpn >= ftl->numLogicalPages)
    {
        set_error("ftl_read: lpn out of range");
        return FTL_ERR_RANGE;
    }
    if (ftl->l2pBlock[lpn] < 0)
    {
        set_error("ftl_read: lpn has never been written (or was trimmed)");
        return FTL_ERR_UNMAPPED;
    }

    NandStatus nst = nand_read_page(ftl->nand, (size_t)ftl->l2pBlock[lpn],
                                    (size_t)ftl->l2pPage[lpn], outData);
    if (nst != NAND_OK)
    {
        set_error(nand_last_error());
        return FTL_ERR_NAND;
    }
    return FTL_OK;
}

FtlStatus ftl_trim(Ftl *ftl, size_t lpn)
{
    if (lpn >= ftl->numLogicalPages)
    {
        set_error("ftl_trim: lpn out of range");
        return FTL_ERR_RANGE;
    }
    if (ftl->l2pBlock[lpn] < 0)
    {
        set_error("ftl_trim: lpn is already unmapped");
        return FTL_ERR_UNMAPPED;
    }

    size_t b = (size_t)ftl->l2pBlock[lpn];
    size_t p = (size_t)ftl->l2pPage[lpn];
    ftl->pageState[b][p] = FTL_PAGE_STALE;
    ftl->physToLpn[b][p] = -1;
    ftl->validCount[b]--;
    ftl->staleCount[b]++;
    ftl->l2pBlock[lpn] = -1;
    ftl->l2pPage[lpn] = -1;

    return FTL_OK;
}

void ftl_print_status(const Ftl *ftl)
{
    const NandFlash *nand = ftl->nand;

    printf("Logical pages: %zu   Physical pages: %zu (blocks=%zu x pages/block=%zu)\n",
           ftl->numLogicalPages, nand->numBlocks * nand->pagesPerBlock,
           nand->numBlocks, nand->pagesPerBlock);
    printf("Free blocks: %zu   Active block: ", ftl->freeBlockCount);
    if (ftl->activeBlock >= 0)
    {

        printf("%lld (next page %zu/%zu)\n", (long long)ftl->activeBlock,
               ftl->activeNextPage, nand->pagesPerBlock);
    }
    else
    {
        printf("none\n");
    }
    printf("GC runs: %llu   Pages relocated: %llu\n",
           (unsigned long long)ftl->gcRuns, (unsigned long long)ftl->pagesRelocated);

    double wa = ftl->hostWrites ? (double)ftl->physicalWrites / (double)ftl->hostWrites : 0.0;
    printf("Host writes: %llu   Physical writes: %llu   Write amplification: %.2fx\n\n",
           (unsigned long long)ftl->hostWrites, (unsigned long long)ftl->physicalWrites, wa);

    printf("%-8s%-10s%-10s%-10s%s\n", "Block", "Erases", "Valid", "Stale",
           "Pages (F=free V=valid S=stale)");
    for (int i = 0; i < 70; ++i)
        putchar('-');
    putchar('\n');

    for (size_t b = 0; b < nand->numBlocks; ++b)
    {
        printf("%-8zu%-10llu%-10zu%-10zu", b,
               (unsigned long long)nand_get_erase_count(nand, b),
               ftl->validCount[b], ftl->staleCount[b]);
        for (size_t p = 0; p < nand->pagesPerBlock; ++p)
        {
            char c = ftl->pageState[b][p] == FTL_PAGE_FREE    ? 'F'
                     : ftl->pageState[b][p] == FTL_PAGE_VALID ? 'V'
                                                              : 'S';
            putchar(c);
        }
        putchar('\n');
    }
}
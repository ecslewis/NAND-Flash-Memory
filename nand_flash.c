#include "nand_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_lastError[256] = "";

/* Simple string-based error setter; call sites build the full message
 * themselves via snprintf before calling this. */
static void set_error_msg(const char *msg) {
    snprintf(g_lastError, sizeof(g_lastError), "%s", msg);
}

const char *nand_last_error(void) {
    return g_lastError;
}

const char *nand_status_str(NandStatus status) {
    switch (status) {
        case NAND_OK: return "OK";
        case NAND_ERR_RANGE: return "index out of range";
        case NAND_ERR_SIZE: return "data size mismatch";
        case NAND_ERR_ALREADY_PROGRAMMED: return "page already programmed (erase required)";
        case NAND_ERR_OUT_OF_ORDER: return "page programmed out of order";
        case NAND_ERR_ALLOC: return "allocation failure";
        default: return "unknown error";
    }
}

static void free_partial(NandFlash *flash, size_t blocksAllocated, size_t pagesAllocatedInLastBlock) {
    if (!flash) return;

    if (flash->storage) {
        for (size_t b = 0; b < blocksAllocated; ++b) {
            size_t pagesInThisBlock = (b == blocksAllocated - 1) ? pagesAllocatedInLastBlock : flash->pagesPerBlock;
            if (flash->storage[b]) {
                for (size_t p = 0; p < pagesInThisBlock; ++p) {
                    free(flash->storage[b][p]);
                }
                free(flash->storage[b]);
            }
        }
        free(flash->storage);
    }
    if (flash->pageStates) {
        for (size_t b = 0; b < blocksAllocated; ++b) free(flash->pageStates[b]);
        free(flash->pageStates);
    }
    free(flash->programmedCount);
    free(flash->stats);
    free(flash);
}

NandFlash *nand_create(size_t numBlocks, size_t pagesPerBlock, size_t pageSizeBytes) {
    if (numBlocks == 0 || pagesPerBlock == 0 || pageSizeBytes == 0) {
        set_error_msg("nand_create: geometry must have non-zero blocks/pages/page size");
        return NULL;
    }

    NandFlash *flash = calloc(1, sizeof(NandFlash));
    if (!flash) {
        set_error_msg("nand_create: allocation failure");
        return NULL;
    }

    flash->numBlocks = numBlocks;
    flash->pagesPerBlock = pagesPerBlock;
    flash->pageSizeBytes = pageSizeBytes;

    flash->storage = calloc(numBlocks, sizeof(uint8_t **));
    flash->pageStates = calloc(numBlocks, sizeof(PageState *));
    flash->programmedCount = calloc(numBlocks, sizeof(size_t));
    flash->stats = calloc(numBlocks, sizeof(BlockStats));

    if (!flash->storage || !flash->pageStates || !flash->programmedCount || !flash->stats) {
        set_error_msg("nand_create: allocation failure");
        free_partial(flash, 0, 0);
        return NULL;
    }

    for (size_t b = 0; b < numBlocks; ++b) {
        flash->storage[b] = calloc(pagesPerBlock, sizeof(uint8_t *));
        flash->pageStates[b] = calloc(pagesPerBlock, sizeof(PageState));
        if (!flash->storage[b] || !flash->pageStates[b]) {
            set_error_msg("nand_create: allocation failure");
            free_partial(flash, b + 1, 0);
            return NULL;
        }

        for (size_t p = 0; p < pagesPerBlock; ++p) {
            flash->storage[b][p] = malloc(pageSizeBytes);
            if (!flash->storage[b][p]) {
                set_error_msg("nand_create: allocation failure");
                free_partial(flash, b + 1, p);
                return NULL;
            }
            memset(flash->storage[b][p], 0xFF, pageSizeBytes);
            flash->pageStates[b][p] = PAGE_ERASED;
        }
    }

    return flash;
}

void nand_destroy(NandFlash *flash) {
    if (!flash) return;
    free_partial(flash, flash->numBlocks, flash->pagesPerBlock);
}

static int check_block_range(const NandFlash *flash, size_t blockIdx) {
    if (blockIdx >= flash->numBlocks) {
        char buf[128];
        snprintf(buf, sizeof(buf), "block index %zu out of range (numBlocks=%zu)",
                  blockIdx, flash->numBlocks);
        set_error_msg(buf);
        return 0;
    }
    return 1;
}

static int check_page_range(const NandFlash *flash, size_t pageIdx) {
    if (pageIdx >= flash->pagesPerBlock) {
        char buf[128];
        snprintf(buf, sizeof(buf), "page index %zu out of range (pagesPerBlock=%zu)",
                  pageIdx, flash->pagesPerBlock);
        set_error_msg(buf);
        return 0;
    }
    return 1;
}

NandStatus nand_erase_block(NandFlash *flash, size_t blockIdx) {
    if (!check_block_range(flash, blockIdx)) return NAND_ERR_RANGE;

    for (size_t p = 0; p < flash->pagesPerBlock; ++p) {
        memset(flash->storage[blockIdx][p], 0xFF, flash->pageSizeBytes);
        flash->pageStates[blockIdx][p] = PAGE_ERASED;
    }
    flash->programmedCount[blockIdx] = 0;
    flash->stats[blockIdx].eraseCount++;
    return NAND_OK;
}

NandStatus nand_program_page(NandFlash *flash, size_t blockIdx, size_t pageIdx,
                              const uint8_t *data, size_t dataLen) {
    if (!check_block_range(flash, blockIdx)) return NAND_ERR_RANGE;
    if (!check_page_range(flash, pageIdx)) return NAND_ERR_RANGE;

    if (dataLen != flash->pageSizeBytes) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                  "programPage: data size %zu does not match page size %zu",
                  dataLen, flash->pageSizeBytes);
        set_error_msg(buf);
        return NAND_ERR_SIZE;
    }

    if (flash->pageStates[blockIdx][pageIdx] != PAGE_ERASED) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                  "programPage: block %zu page %zu is already programmed -- erase the "
                  "block before rewriting it (NAND can only flip bits 1->0; it cannot "
                  "be reprogrammed in place)",
                  blockIdx, pageIdx);
        set_error_msg(buf);
        return NAND_ERR_ALREADY_PROGRAMMED;
    }

    if (pageIdx != flash->programmedCount[blockIdx]) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                  "programPage: block %zu page %zu programmed out of order -- next "
                  "page to program must be %zu (NAND requires sequential page "
                  "programming within a block)",
                  blockIdx, pageIdx, flash->programmedCount[blockIdx]);
        set_error_msg(buf);
        return NAND_ERR_OUT_OF_ORDER;
    }

    memcpy(flash->storage[blockIdx][pageIdx], data, dataLen);
    flash->pageStates[blockIdx][pageIdx] = PAGE_PROGRAMMED;
    flash->programmedCount[blockIdx]++;
    return NAND_OK;
}

NandStatus nand_read_page(const NandFlash *flash, size_t blockIdx, size_t pageIdx,
                           const uint8_t **outData) {
    if (!check_block_range(flash, blockIdx)) return NAND_ERR_RANGE;
    if (!check_page_range(flash, pageIdx)) return NAND_ERR_RANGE;

    *outData = flash->storage[blockIdx][pageIdx];
    return NAND_OK;
}

PageState nand_get_page_state(const NandFlash *flash, size_t blockIdx, size_t pageIdx) {
    /* Caller is expected to pass valid indices; range-checked variants
     * (nand_read_page etc.) are available where errors must be reported. */
    return flash->pageStates[blockIdx][pageIdx];
}

uint64_t nand_get_erase_count(const NandFlash *flash, size_t blockIdx) {
    return flash->stats[blockIdx].eraseCount;
}

size_t nand_next_programmable_page(const NandFlash *flash, size_t blockIdx) {
    return flash->programmedCount[blockIdx];
}

void nand_print_status(const NandFlash *flash) {
    printf("%-8s%-12s%s\n", "Block", "Erases", "Pages (E=erased, P=programmed)");
    for (int i = 0; i < 60; ++i) putchar('-');
    putchar('\n');

    for (size_t b = 0; b < flash->numBlocks; ++b) {
        printf("%-8zu%-12llu", b, (unsigned long long)flash->stats[b].eraseCount);
        for (size_t p = 0; p < flash->pagesPerBlock; ++p) {
            putchar(flash->pageStates[b][p] == PAGE_ERASED ? 'E' : 'P');
        }
        putchar('\n');
    }
}

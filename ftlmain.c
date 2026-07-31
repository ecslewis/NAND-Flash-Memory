#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ftl.h"
#include "nand_flash.h"

static uint8_t *string_to_page(const char *text, size_t pageSize)
{
    uint8_t *buf = malloc(pageSize);
    memset(buf, 0xFF, pageSize);
    size_t len = strlen(text);
    if (len > pageSize)
        len = pageSize;
    memcpy(buf, text, len);
    return buf;
}

static void print_page_contents(const uint8_t *data, size_t pageSize)
{
    for (size_t i = 0; i < pageSize; ++i)
    {
        if (data[i] == 0xFF)
            break;
        putchar((char)data[i]);
    }
}

static void run_demo(Ftl *ftl, size_t pageSize)
{
    printf("=== FTL Simulator: scripted demo ===\n\n");

    printf("-- Writing logical pages 0..4 --\n");
    for (size_t lpn = 0; lpn < 5; ++lpn)
    {
        char text[64];
        snprintf(text, sizeof(text), "value for lpn %zu (v1)", lpn);
        uint8_t *data = string_to_page(text, pageSize);
        ftl_write(ftl, lpn, data, pageSize);
        free(data);
    }

    {
        const uint8_t *data;
        ftl_read(ftl, 2, &data);
        printf("Read lpn 2: \"");
        print_page_contents(data, pageSize);
        printf("\"\n\n");
    }

    printf("-- Status after initial writes --\n");
    ftl_print_status(ftl);
    printf("\n");

    printf("-- Rewriting lpn 0 several times (out-of-place updates create garbage) --\n");
    for (int i = 2; i <= 6; ++i)
    {
        char text[64];
        snprintf(text, sizeof(text), "value for lpn 0 (v%d)", i);
        uint8_t *data = string_to_page(text, pageSize);
        ftl_write(ftl, 0, data, pageSize);
        free(data);
    }

    {
        const uint8_t *data;
        ftl_read(ftl, 0, &data);
        printf("Read lpn 0 (should show the latest version): \"");
        print_page_contents(data, pageSize);
        printf("\"\n\n");
    }

    printf("-- Status after rewrites (note stale pages piling up in old block(s)) --\n");
    ftl_print_status(ftl);
    printf("\n");

    printf("-- Trimming lpn 1 (like a delete/discard) --\n");
    ftl_trim(ftl, 1);
    {
        const uint8_t *data;
        FtlStatus st = ftl_read(ftl, 1, &data);
        if (st != FTL_OK)
        {
            printf("  Read after trim correctly failed: %s\n\n", ftl_last_error());
        }
    }

    printf("-- Forcing a garbage collection pass --\n");
    FtlStatus gcSt = ftl_force_gc(ftl);
    if (gcSt == FTL_OK)
    {
        printf("  GC reclaimed a block successfully.\n\n");
    }
    else
    {
        printf("  GC: %s\n\n", ftl_last_error());
    }

    printf("-- Status after forced GC --\n");
    ftl_print_status(ftl);
    printf("\n");

    printf("-- Confirming data survived GC relocation --\n");
    {
        const uint8_t *data;
        ftl_read(ftl, 0, &data);
        printf("Read lpn 0: \"");
        print_page_contents(data, pageSize);
        printf("\"\n");
        ftl_read(ftl, 3, &data);
        printf("Read lpn 3: \"");
        print_page_contents(data, pageSize);
        printf("\"\n\n");
    }
}

static void print_help(void)
{
    printf(
        "Commands:\n"
        "  write <lpn> <text>   Write text to a logical page\n"
        "  read <lpn>           Read a logical page\n"
        "  trim <lpn>           Mark a logical page deleted (like TRIM)\n"
        "  gc                   Force a garbage-collection pass\n"
        "  status               Show FTL status (mapping, GC stats, write amplification)\n"
        "  help                 Show this help\n"
        "  quit                 Exit\n");
}

static void run_interactive(Ftl *ftl, size_t pageSize)
{
    printf("\n=== Interactive mode ===\n");
    printf("Logical pages available: 0..%zu\n", ftl->numLogicalPages - 1);
    print_help();

    char line[512];
    printf("\n> ");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin))
    {
        line[strcspn(line, "\n")] = '\0';

        char cmd[32] = {0};
        int consumed = 0;
        sscanf(line, "%31s%n", cmd, &consumed);

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0)
        {
            break;
        }
        else if (strcmp(cmd, "help") == 0)
        {
            print_help();
        }
        else if (strcmp(cmd, "status") == 0)
        {
            ftl_print_status(ftl);
        }
        else if (strcmp(cmd, "gc") == 0)
        {
            FtlStatus st = ftl_force_gc(ftl);
            if (st == FTL_OK)
            {
                printf("GC reclaimed a block.\n");
            }
            else
            {
                printf("GC: %s\n", ftl_last_error());
            }
        }
        else if (strcmp(cmd, "write") == 0)
        {
            size_t lpn;
            int n = 0;
            if (sscanf(line + consumed, "%zu%n", &lpn, &n) != 1)
            {
                printf("Usage: write <lpn> <text>\n");
            }
            else
            {
                const char *text = line + consumed + n;
                while (*text == ' ')
                    text++;
                uint8_t *data = string_to_page(text, pageSize);
                FtlStatus st = ftl_write(ftl, lpn, data, pageSize);
                free(data);
                if (st == FTL_OK)
                {
                    printf("Wrote lpn %zu.\n", lpn);
                }
                else
                {
                    printf("Error: %s\n", ftl_last_error());
                }
            }
        }
        else if (strcmp(cmd, "read") == 0)
        {
            size_t lpn;
            if (sscanf(line + consumed, "%zu", &lpn) != 1)
            {
                printf("Usage: read <lpn>\n");
            }
            else
            {
                const uint8_t *data;
                FtlStatus st = ftl_read(ftl, lpn, &data);
                if (st == FTL_OK)
                {
                    printf("lpn %zu: \"", lpn);
                    print_page_contents(data, pageSize);
                    printf("\"\n");
                }
                else
                {
                    printf("Error: %s\n", ftl_last_error());
                }
            }
        }
        else if (strcmp(cmd, "trim") == 0)
        {
            size_t lpn;
            if (sscanf(line + consumed, "%zu", &lpn) != 1)
            {
                printf("Usage: trim <lpn>\n");
            }
            else
            {
                FtlStatus st = ftl_trim(ftl, lpn);
                if (st == FTL_OK)
                {
                    printf("Trimmed lpn %zu.\n", lpn);
                }
                else
                {
                    printf("Error: %s\n", ftl_last_error());
                }
            }
        }
        else if (strlen(cmd) == 0)
        {
            /* blank line, ignore */
        }
        else
        {
            printf("Unknown command. Type 'help' for a list.\n");
        }

        printf("\n> ");
        fflush(stdout);
    }
}

int main(void)
{
    /* 8 blocks x 8 pages x 32 bytes = 64 physical pages. Reserve 2 blocks
     * (16 pages) as spare over-provisioning for GC, leaving 48 logical
     * pages -- a 25% over-provisioning ratio, in the same ballpark as a
     * real SSD. */
    NandFlash *nand = nand_create(8, 8, 32);
    if (!nand)
    {
        fprintf(stderr, "Failed to create NandFlash: %s\n", nand_last_error());
        return 1;
    }

    size_t numLogicalPages = ftl_recommended_logical_pages(nand, 2);
    Ftl *ftl = ftl_create(nand, numLogicalPages);
    if (!ftl)
    {
        fprintf(stderr, "Failed to create FTL: %s\n", ftl_last_error());
        nand_destroy(nand);
        return 1;
    }

    run_demo(ftl, nand->pageSizeBytes);
    run_interactive(ftl, nand->pageSizeBytes);

    ftl_destroy(ftl);
    nand_destroy(nand);
    printf("Goodbye!\n");
    return 0;
}
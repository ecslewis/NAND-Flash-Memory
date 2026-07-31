#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nand_flash.h"

/* Converts a C string into a page-sized buffer, padding the remainder
 * with 0xFF (the "unwritten" value). Caller must free() the result. */
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

/* Prints a page's contents up to the first 0xFF byte (treated as the
 * end of written data), for readable demo output. */
static void print_page_contents(const uint8_t *data, size_t pageSize)
{
    for (size_t i = 0; i < pageSize; ++i)
    {
        if (data[i] == 0xFF)
            break;
        putchar((char)data[i]);
    }
}

static void run_demo(NandFlash *flash)
{
    printf("=== NAND Flash Simulator: scripted demo ===\n\n");

    printf("-- Erasing block 0 --\n");
    nand_erase_block(flash, 0);

    printf("-- Programming pages 0 and 1 in block 0 --\n");
    {
        uint8_t *p0 = string_to_page("Hello, NAND!", flash->pageSizeBytes);
        uint8_t *p1 = string_to_page("Second page.", flash->pageSizeBytes);
        nand_program_page(flash, 0, 0, p0, flash->pageSizeBytes);
        nand_program_page(flash, 0, 1, p1, flash->pageSizeBytes);
        free(p0);
        free(p1);
    }

    {
        const uint8_t *data;
        nand_read_page(flash, 0, 0, &data);
        printf("Read back page 0: \"");
        print_page_contents(data, flash->pageSizeBytes);
        printf("\"\n");

        nand_read_page(flash, 0, 1, &data);
        printf("Read back page 1: \"");
        print_page_contents(data, flash->pageSizeBytes);
        printf("\"\n\n");
    }

    printf("-- Trying to reprogram page 0 without erasing (should fail) --\n");
    {
        uint8_t *attempt = string_to_page("Overwrite attempt", flash->pageSizeBytes);
        NandStatus st = nand_program_page(flash, 0, 0, attempt, flash->pageSizeBytes);
        free(attempt);
        if (st != NAND_OK)
        {
            printf("  Caught expected error: %s\n\n", nand_last_error());
        }
    }

    printf("-- Trying to program page 3 while page 2 is still erased (out of order) --\n");
    {
        uint8_t *attempt = string_to_page("Skipping ahead", flash->pageSizeBytes);
        NandStatus st = nand_program_page(flash, 0, 3, attempt, flash->pageSizeBytes);
        free(attempt);
        if (st != NAND_OK)
        {
            printf("  Caught expected error: %s\n\n", nand_last_error());
        }
    }

    printf("-- Erasing block 0 again clears everything --\n");
    nand_erase_block(flash, 0);
    {
        const uint8_t *data;
        nand_read_page(flash, 0, 0, &data);
        printf("Read back page 0 after erase (should be empty/0xFF): \"");
        print_page_contents(data, flash->pageSizeBytes);
        printf("\"\n\n");
    }

    printf("-- Status after demo --\n");
    nand_print_status(flash);
    printf("\n");
}

static void print_help(void)
{
    printf(
        "Commands:\n"
        "  erase <block>                  Erase an entire block\n"
        "  program <block> <page> <text>  Program a page with text\n"
        "  read <block> <page>            Read and print a page's contents\n"
        "  status                         Show block/page status table\n"
        "  size                           Show storage used vs. total capacity\n"
        "  help                           Show this help\n"
        "  quit                           Exit\n");
}

static void run_interactive(NandFlash *flash)
{
    printf("\n=== Interactive mode ===\n");
    printf("Geometry: %zu blocks x %zu pages x %zu bytes/page\n",
           flash->numBlocks, flash->pagesPerBlock, flash->pageSizeBytes);
    print_help();

    char line[512];
    printf("\n> ");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin))
    {
        /* strip trailing newline */
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
            nand_print_status(flash);
        }
        else if (strcmp(cmd, "size") == 0)
        {
            size_t used = nand_used_bytes(flash);
            size_t total = nand_total_capacity_bytes(flash);
            double pct = total ? (100.0 * (double)used / (double)total) : 0.0;
            printf("Storage used: %zu / %zu bytes (%.1f%%)\n", used, total, pct);
        }
        else if (strcmp(cmd, "erase") == 0)
        {
            size_t block;
            if (sscanf(line + consumed, "%zu", &block) != 1)
            {
                printf("Usage: erase <block>\n");
            }
            else
            {
                NandStatus st = nand_erase_block(flash, block);
                if (st == NAND_OK)
                {
                    printf("Erased block %zu. Erase count now %llu.\n",
                           block, (unsigned long long)nand_get_erase_count(flash, block));
                }
                else
                {
                    printf("Error: %s\n", nand_last_error());
                }
            }
        }
        else if (strcmp(cmd, "program") == 0)
        {
            size_t block, page;
            int n = 0;
            if (sscanf(line + consumed, "%zu %zu%n", &block, &page, &n) != 2)
            {
                printf("Usage: program <block> <page> <text>\n");
            }
            else
            {
                const char *text = line + consumed + n;
                while (*text == ' ')
                    text++; /* skip leading space before text */
                uint8_t *data = string_to_page(text, flash->pageSizeBytes);
                NandStatus st = nand_program_page(flash, block, page, data, flash->pageSizeBytes);
                free(data);
                if (st == NAND_OK)
                {
                    printf("Programmed block %zu page %zu.\n", block, page);
                }
                else
                {
                    printf("Error: %s\n", nand_last_error());
                }
            }
        }
        else if (strcmp(cmd, "read") == 0)
        {
            size_t block, page;
            if (sscanf(line + consumed, "%zu %zu", &block, &page) != 2)
            {
                printf("Usage: read <block> <page>\n");
            }
            else
            {
                const uint8_t *data;
                NandStatus st = nand_read_page(flash, block, page, &data);
                if (st == NAND_OK)
                {
                    printf("Block %zu page %zu: \"", block, page);
                    print_page_contents(data, flash->pageSizeBytes);
                    printf("\"\n");
                }
                else
                {
                    printf("Error: %s\n", nand_last_error());
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
    /* Small geometry for a readable demo: 4 blocks, 4 pages/block, 32 bytes/page. */
    NandFlash *flash = nand_create(4, 4, 32);
    if (!flash)
    {
        fprintf(stderr, "Failed to create flash: %s\n", nand_last_error());
        return 1;
    }

    run_demo(flash);
    run_interactive(flash);

    nand_destroy(flash);
    printf("Goodbye!\n");
    return 0;
}
# NAND Flash Simulator (C)

A small, dependency-free C11 simulation of NAND flash memory behavior at the
block/page level

## Table of contents

- [What it models](#what-it-models)
- [Files](#files)
- [Build & run](#build--run)
- [Interactive commands](#interactive-commands)
- [Example session](#example-session)
- [API sketch](#api-sketch)
- [Ideas for extending it](#ideas-for-extending-it)
- [Notes on the C port](#notes-on-the-c-port)
- [License](#license)

## What it models

Real NAND flash has three quirky rules that make it different from RAM or a
plain file, and this simulator enforces all three:

1. **Erase granularity is a block, not a page.** Erasing sets every byte in
   every page of a block to `0xFF` (all bits 1).
2. **Programming can only flip bits 1 → 0.** In practice this means a page
   must be freshly erased before you can program it — you can't just
   overwrite it like a normal file. Trying to reprogram an already-programmed
   page returns `NAND_ERR_ALREADY_PROGRAMMED`.
3. **Pages must be programmed sequentially within a block** (page 0, then
   page 1, then page 2, ...). Programming out of order returns
   `NAND_ERR_OUT_OF_ORDER`, matching real NAND datasheet restrictions.

Reading, on the other hand, is unrestricted — any page can be read at any
time, returning whatever is currently stored (including `0xFF` for
never-programmed pages).

## Files

```
.
├── nand_flash.h    # NandFlash type + function declarations
├── nand_flash.c    # implementation: create/destroy, erase, program, read
├── main.c          # scripted demo + interactive CLI
├── Makefile        # build targets (see below)
└── README.md
```

Errors are reported via `NandStatus` return codes plus `nand_last_error()`
for a detailed message — the conventional C idiom, since C has no
exceptions.

## Build & run

Requires `gcc` (or another C11 compiler — edit `CC` in the Makefile) and
`make`.

```bash
make          # build ./nand_sim
make run      # build (if needed) and run it
```

Other targets:

| Target       | Purpose                                                        |
|--------------|-----------------------------------------------------------------|
| `make`       | Optimized build (`-O2`) with warnings on                        |
| `make run`   | Build then run                                                  |
| `make debug` | Rebuild with `-g -O0`, good for stepping through with `gdb`      |
| `make asan`  | Rebuild with AddressSanitizer + UBSan to catch memory/UB bugs    |
| `make clean` | Remove object files and the binary                              |

Without `make`, you can compile directly:

```bash
gcc -std=c11 -Wall -Wextra -O2 -o nand_sim main.c nand_flash.c
```

## Interactive commands

The program first runs a short scripted demo (erasing, programming, and
deliberately triggering both error cases), then drops you into a CLI:

```
erase <block>                  Erase an entire block
program <block> <page> <text>  Program a page with text
read <block> <page>            Read and print a page's contents
status                         Show block/page status table
help                           Show this help
quit                           Exit
```

Default geometry (set in `main()`): 4 blocks × 4 pages/block × 32 bytes/page —
small enough to explore by hand. Change the `nand_create(...)` args in
`main.c` to try a bigger, more realistic layout (e.g. `nand_create(128, 64,
2048)`).

## Example session

```
> erase 1
Erased block 1. Erase count now 1.

> program 1 0 test data here
Programmed block 1 page 0.

> read 1 0
Block 1 page 0: "test data here"

> program 1 0 overwrite
Error: programPage: block 1 page 0 is already programmed -- erase the block
before rewriting it (NAND can only flip bits 1->0; it cannot be reprogrammed
in place)
```

## API sketch

```c
NandFlash *nand_create(size_t numBlocks, size_t pagesPerBlock, size_t pageSizeBytes);
void nand_destroy(NandFlash *flash);

NandStatus nand_erase_block(NandFlash *flash, size_t blockIdx);
NandStatus nand_program_page(NandFlash *flash, size_t blockIdx, size_t pageIdx,
                              const uint8_t *data, size_t dataLen);
NandStatus nand_read_page(const NandFlash *flash, size_t blockIdx, size_t pageIdx,
                           const uint8_t **outData);

const char *nand_last_error(void); /* detailed message for the last failed call */
```

All fallible functions return a `NandStatus` (`NAND_OK` on success); check
`nand_last_error()` for details when something other than `NAND_OK` comes
back.

## Ideas for extending it

- **Wear leveling**: track erase counts (already collected in `BlockStats`)
  and pick the least-worn free block when writing new data.
- **Bad block management**: mark blocks bad after too many erases and skip
  them.
- **Partial-page bit masking**: instead of requiring a full erase before any
  rewrite, allow reprogramming as long as the new byte only clears bits that
  are still 1 in the old byte (`old & new == new`), and reject the write
  otherwise — this is the literal bit-level rule real NAND enforces.
- **ECC / bit-flip simulation**: randomly flip a bit occasionally to model
  charge leakage / retention errors, and add simple parity or Hamming-code
  error correction on read.
- **An FTL layer** on top: logical-to-physical page mapping, garbage
  collection, and a simple key-value store backed by the flash model.

Contributions and forks exploring any of the above are welcome.

## Notes on the C port

- Uses manual `malloc`/`free` with three-level pointer indirection
  (`storage[block][page][byte]`); `nand_destroy()` frees everything.
- Clean under `-Wall -Wextra` with no warnings, and clean under
  AddressSanitizer/UBSan (`make asan`) with no leaks or errors.
- Error reporting uses return codes (`NandStatus`) instead of C++
  exceptions, plus a `nand_last_error()` string for a detailed message.

## License

No license has been chosen yet — add one (e.g. MIT) before publishing if you
want others to be able to reuse this freely.

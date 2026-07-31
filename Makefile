CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2

NAND_TARGET := nand_sim
NAND_SRCS   := main.c nand_flash.c
NAND_OBJS   := $(NAND_SRCS:.c=.o)

FTL_TARGET := ftl_sim
FTL_SRCS   := ftl_main.c nand_flash.c ftl.c
FTL_OBJS   := $(FTL_SRCS:.c=.o)

.PHONY: all clean run run-ftl debug asan

all: $(NAND_TARGET) $(FTL_TARGET)

$(NAND_TARGET): $(NAND_OBJS)
	$(CC) $(CFLAGS) -o $@ $(NAND_OBJS)

$(FTL_TARGET): $(FTL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(FTL_OBJS)

%.o: %.c nand_flash.h ftl.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(NAND_TARGET)
	./$(NAND_TARGET)

run-ftl: $(FTL_TARGET)
	./$(FTL_TARGET)

# Debug build with symbols, no optimization
debug: CFLAGS := -std=c11 -Wall -Wextra -g -O0
debug: clean all

# Build with AddressSanitizer/UBSan for memory/UB checking
asan: CFLAGS := -std=c11 -Wall -Wextra -g -O0 -fsanitize=address,undefined
asan: clean all

clean:
	rm -f $(NAND_OBJS) $(FTL_OBJS) $(NAND_TARGET) $(FTL_TARGET)
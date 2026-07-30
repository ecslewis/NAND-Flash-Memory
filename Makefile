CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2
TARGET  := nand_sim
SRCS    := main.c nand_flash.c
OBJS    := $(SRCS:.c=.o)

.PHONY: all clean run debug asan

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c nand_flash.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

# Debug build with symbols, no optimization
debug: CFLAGS := -std=c11 -Wall -Wextra -g -O0
debug: clean $(TARGET)

# Build with AddressSanitizer/UBSan for memory/UB checking
asan: CFLAGS := -std=c11 -Wall -Wextra -g -O0 -fsanitize=address,undefined
asan: clean $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
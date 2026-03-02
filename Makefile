CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11
TARGET = 68k-emu

SRCS = main.c cpu.c memory.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# Regression tests: run all built-in tests and verify expected output
test: $(TARGET)
	@echo "Running regression tests..."
	@failed=0; \
	output=$$(./$(TARGET) 2>&1); echo "$$output" | grep -q "PC=0x00000012" && echo "  nop:     PASS" || { echo "  nop:     FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move 2>&1); echo "$$output" | grep -q "D2=0x00000000" && echo "  move:   PASS" || { echo "  move:   FAIL"; failed=1; }; \
	output=$$(./$(TARGET) moveq 2>&1); echo "$$output" | grep -q "D0=0x0000002A" && echo "$$output" | grep -q "D1=0xFFFFFFFF" && echo "  moveq:  PASS" || { echo "  moveq:  FAIL"; failed=1; }; \
	output=$$(./$(TARGET) add 2>&1); echo "$$output" | grep -q "D1=0x0000002A" && echo "  add:    PASS" || { echo "  add:    FAIL"; failed=1; }; \
	output=$$(./$(TARGET) sub 2>&1); echo "$$output" | grep -q "D1=0x0000002A" && echo "  sub:    PASS" || { echo "  sub:    FAIL"; failed=1; }; \
	output=$$(./$(TARGET) cmp 2>&1); echo "$$output" | grep -q "SR=0x2704" && echo "  cmp:    PASS" || { echo "  cmp:    FAIL"; failed=1; }; \
	output=$$(./$(TARGET) bcc 2>&1); echo "$$output" | grep -q "D2=0x00000002" && echo "  bcc:    PASS" || { echo "  bcc:    FAIL"; failed=1; }; \
	output=$$(./$(TARGET) bcc_all 2>&1); echo "$$output" | grep -q "D2=0x0000000F" && echo "  bcc_all: PASS" || { echo "  bcc_all: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) bsr_rts 2>&1); echo "$$output" | grep -q "D2=0x0000002A" && echo "  bsr_rts: PASS" || { echo "  bsr_rts: FAIL"; failed=1; }; \
	if [ $$failed -eq 0 ]; then echo "All tests passed."; else echo "Some tests failed."; exit 1; fi

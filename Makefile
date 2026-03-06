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
	output=$$(./$(TARGET) move_mem 2>&1); echo "$$output" | grep -q "D1=0x0000002A" && echo "  move_mem: PASS" || { echo "  move_mem: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move_w 2>&1); echo "$$output" | grep -q "D1=0x0000002A" && echo "  move_w:   PASS" || { echo "  move_w:   FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move_b 2>&1); echo "$$output" | grep -q "D1=0x0000002A" && echo "  move_b:   PASS" || { echo "  move_b:   FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move_w_mem 2>&1); echo "$$output" | grep -q "D1=0x0000FFFF" && echo "  move_w_mem: PASS" || { echo "  move_w_mem: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move_b_mem 2>&1); echo "$$output" | grep -q "D1=0x000000AB" && echo "  move_b_mem: PASS" || { echo "  move_b_mem: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move_imm 2>&1); echo "$$output" | grep -q "D0=0x12345678" && echo "  move_imm: PASS" || { echo "  move_imm: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move_imm_mem 2>&1); echo "$$output" | grep -q "D1=0xDEADBEEF" && echo "  move_imm_mem: PASS" || { echo "  move_imm_mem: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move_anp 2>&1); echo "$$output" | grep -q "D1=0x12345678" && echo "$$output" | grep -q "A7=0x00001004" && echo "  move_anp: PASS" || { echo "  move_anp: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) move_disp 2>&1); echo "$$output" | grep -q "D1=0x12345678" && echo "  move_disp: PASS" || { echo "  move_disp: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) moveq 2>&1); echo "$$output" | grep -q "D0=0x0000002A" && echo "$$output" | grep -q "D1=0xFFFFFFFF" && echo "  moveq:  PASS" || { echo "  moveq:  FAIL"; failed=1; }; \
	output=$$(./$(TARGET) add 2>&1); echo "$$output" | grep -q "D1=0x0000002A" && echo "  add:    PASS" || { echo "  add:    FAIL"; failed=1; }; \
	output=$$(./$(TARGET) sub 2>&1); echo "$$output" | grep -q "D1=0x0000002A" && echo "  sub:    PASS" || { echo "  sub:    FAIL"; failed=1; }; \
	output=$$(./$(TARGET) cmp 2>&1); echo "$$output" | grep -q "SR=0x2704" && echo "  cmp:    PASS" || { echo "  cmp:    FAIL"; failed=1; }; \
	output=$$(./$(TARGET) bcc 2>&1); echo "$$output" | grep -q "D2=0x00000002" && echo "  bcc:    PASS" || { echo "  bcc:    FAIL"; failed=1; }; \
	output=$$(./$(TARGET) bcc_all 2>&1); echo "$$output" | grep -q "D2=0x0000000F" && echo "  bcc_all: PASS" || { echo "  bcc_all: FAIL"; failed=1; }; \
	output=$$(./$(TARGET) bsr_rts 2>&1); echo "$$output" | grep -q "D2=0x0000002A" && echo "  bsr_rts: PASS" || { echo "  bsr_rts: FAIL"; failed=1; }; \
	if [ $$failed -eq 0 ]; then echo "All tests passed."; else echo "Some tests failed."; exit 1; fi

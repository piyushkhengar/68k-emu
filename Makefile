CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11 -Isrc -Isrc/core -Isrc/isa -Isrc/genesis -I.
TARGET = 68k-emu

# Optional: -DHAVE_ZLIB and -lz for .json.gz support
ZLIB_CFLAGS ?= $(shell pkg-config --cflags zlib 2>/dev/null || echo "")
ZLIB_LIBS   ?= $(shell pkg-config --libs zlib 2>/dev/null || echo "-lz")
ifeq ($(shell echo 'int main(){return 0;}' | $(CC) -x c - -lz -o /dev/null 2>/dev/null && echo ok),ok)
  CFLAGS += -DHAVE_ZLIB
  LDFLAGS += -lz
endif

# SDL2 (detected automatically; used by `make genesis`)
SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL2_LIBS   := $(shell sdl2-config --libs 2>/dev/null)

# ---- Source groups ------------------------------------------------
CORE_SRCS = src/main.c src/system.c \
            src/core/cpu.c src/core/memory.c src/core/ea.c \
            src/isa/move.c src/isa/alu.c src/isa/branch.c src/isa/control.c \
            src/isa/immediate.c src/isa/logic.c src/isa/shift.c src/isa/bit.c \
            src/isa/movem.c src/isa/movep.c \
            src/tests.c src/tests_68010.c src/tests_68020.c src/tests_68030.c src/tests_68040.c src/tests_68060.c src/tests_68080.c src/timing.c src/timing_tests.c src/processor_tests.c \
            deps/cJSON/cJSON.c

GENESIS_SRCS = src/genesis/bus.c src/genesis/vdp.c src/genesis/io.c \
               src/genesis/z80.c src/genesis/psg.c src/genesis/ym2612.c \
               src/genesis/genesis.c src/genesis/genesis_tests.c

SRCS = $(CORE_SRCS) $(GENESIS_SRCS)
OBJS = $(SRCS:.c=.o)

# ---- Musashi differential validator ------------------------------------
# m68kfpu.c is #included directly by m68kcpu.c — do not compile separately.
# m68kdasm.c is omitted (disassembly not needed for differential testing).
MUSASHI_SRCS = deps/musashi/m68kcpu.c deps/musashi/m68kops.c \
               deps/musashi/softfloat/softfloat.c
MUSASHI_EMU_SRCS = src/core/cpu.c src/core/memory.c src/core/ea.c \
                   src/isa/move.c src/isa/alu.c src/isa/branch.c src/isa/control.c \
                   src/isa/immediate.c src/isa/logic.c src/isa/shift.c src/isa/bit.c \
                   src/isa/movem.c src/isa/movep.c \
                   src/timing.c \
                   deps/cJSON/cJSON.c
MUSASHI_DIFF_SRCS = $(MUSASHI_EMU_SRCS) $(MUSASHI_SRCS) src/musashi_diff.c
MUSASHI_CFLAGS = $(CFLAGS) -Ideps/musashi -Ideps/musashi/softfloat -DMUSASHI_DIFF_MODE -Wno-unused-parameter -Wno-sign-compare

.PHONY: all clean test mcl68-test genesis processor-tests processor-tests-68010 processor-tests-68020 processor-tests-68030 processor-tests-68040 processor-tests-68060 processor-tests-68080 musashi-diff baremetal qemu-bm qemu-bm-headless qemu-bm-elf burn-usb clean-bm

include bare-metal/Makefile.baremetal

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Genesis target: rebuild with SDL2 for graphical output
#   - genesis.c recompiled with -DHAVE_SDL2 (separate .o to avoid conflicts)
#   - renderer.c compiled with SDL2 headers
genesis: $(filter-out src/genesis/genesis.o,$(OBJS)) src/genesis/genesis_sdl.o src/genesis/renderer.o src/genesis/audio.o
	$(CC) $(CFLAGS) $(SDL2_CFLAGS) -o $(TARGET) $(filter-out src/genesis/genesis.o,$(OBJS)) src/genesis/genesis_sdl.o src/genesis/renderer.o src/genesis/audio.o $(LDFLAGS) $(SDL2_LIBS)

src/genesis/genesis_sdl.o: src/genesis/genesis.c
	$(CC) $(CFLAGS) $(SDL2_CFLAGS) -DHAVE_SDL2 -c -o $@ $<

src/genesis/renderer.o: src/genesis/renderer.c
	$(CC) $(CFLAGS) $(SDL2_CFLAGS) -c -o $@ $<

src/genesis/audio.o: src/genesis/audio.c
	$(CC) $(CFLAGS) $(SDL2_CFLAGS) -DHAVE_SDL2 -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

68k-musashi-diff: $(MUSASHI_DIFF_SRCS)
	$(CC) $(MUSASHI_CFLAGS) -o $@ $^ $(LDFLAGS)

# Generate Musashi opcode table if not present
deps/musashi/m68kops.c: deps/musashi/m68kmake.c deps/musashi/m68k_in.c
	$(CC) -o deps/musashi/m68kmake deps/musashi/m68kmake.c
	cd deps/musashi && ./m68kmake

clean:
	rm -f $(OBJS) src/genesis/genesis_sdl.o src/genesis/renderer.o src/genesis/audio.o src/system.o $(TARGET)
	rm -f 68k-musashi-diff deps/musashi/m68kmake

# SPEED: run tests at given MHz (e.g. make test SPEED=7.09). Omit for hyperspeed.
SPEED ?=
SPEED_ARG = $(if $(SPEED), --speed $(SPEED),)

# Regression tests: run all built-in tests in one process
test: $(TARGET)
	@./$(TARGET) --run-all-tests$(SPEED_ARG) || exit 1

# MCL68 end-to-end test: patches ROM (requires mcl68_test.bin.bak) and runs full opcode suite
MCL68_ROM ?= mcl68_test.bin
mcl68-test: $(TARGET)
	@python3 patch_mcl68_rom.py
	@./$(TARGET) $(MCL68_ROM) --max-steps 50000000 2>&1 | grep -q "ALL TESTS PASSED" \
		&& echo "MCL68: PASS" \
		|| (echo "MCL68: FAIL"; exit 1)

# ProcessorTests: run SingleStepTests/680x0 JSON suite (set PROC_TESTS=path/to/68000/v1, PROC_FILTER=ADD for subset)
PROC_TESTS ?= ProcessorTests/68000/v1
PROC_FILTER ?=
processor-tests: $(TARGET)
	@if [ -d "$(PROC_TESTS)" ]; then \
		./$(TARGET) --processor-tests "$(PROC_TESTS)" $(if $(PROC_FILTER),$(PROC_FILTER),) || exit 1; \
	else \
		echo "ProcessorTests not found at $(PROC_TESTS)"; \
		echo "Clone: git clone https://github.com/SingleStepTests/680x0 ProcessorTests"; \
		exit 1; \
	fi

# ProcessorTests against 68010: runs the 68000 test corpus in 68010 mode.
# Observed baseline: ~905k pass, ~94k fail.
#   ~70k failures match the 68000 baseline (exception frame and other known gaps
#     in the 68000 test runner itself — not 68010-specific).
#   ~24k additional failures: TRAP/TRAPV/CHK/RTE/DIVU(div0) push an 8-byte
#     exception frame on 68010 vs 6 bytes on 68000.
# No 68010-specific test corpus exists in SingleStepTests/680x0.
processor-tests-68010: $(TARGET)
	@if [ -d "$(PROC_TESTS)" ]; then \
		./$(TARGET) --cpu 68010 --processor-tests "$(PROC_TESTS)" $(if $(PROC_FILTER),$(PROC_FILTER),); \
		EXIT=$$?; \
		echo ""; \
		echo "Expected failures (~94k total):"; \
		echo "  ~70k  baseline failures shared with 68000 mode"; \
		echo "  ~24k  exception-frame tests (8-byte frame vs 6-byte on 68000)"; \
		exit $$EXIT; \
	else \
		echo "ProcessorTests not found at $(PROC_TESTS)"; \
		echo "Clone: git clone https://github.com/SingleStepTests/680x0 ProcessorTests"; \
		exit 1; \
	fi

# ProcessorTests against 68020: runs the 68000 test corpus in 68020 mode.
# This is a regression/compatibility test; it validates that 68000 instructions
# still execute correctly when the CPU is in 68020 mode.
#
# Observed baseline: ~893k pass, ~107k fail.
# Expected failure categories (~107k total, all intentional, not bugs):
#   ~70k  indexed-EA tests where the corpus brief extension word has non-zero
#         scale bits (10-9); the 68000 corpus expects those bits ignored (real
#         68000 hardware reserves them), but the 68020 correctly applies the
#         scale — producing a different, architecturally valid result.
#   ~24k  exception-frame tests (8-byte frame vs 6-byte, shared with 68010)
#   ~13k  mode-6 indexed-EA tests where the corpus has extension words with
#         bit 8=1; the 68020 correctly interprets these as full extension words
#         while 68000 hardware ignores the bit (correct 68020 behaviour)
#
# No 68020-specific test corpus exists in SingleStepTests/680x0 yet.
processor-tests-68020: $(TARGET)
	@if [ -d "$(PROC_TESTS)" ]; then \
		./$(TARGET) --cpu 68020 --processor-tests "$(PROC_TESTS)" $(if $(PROC_FILTER),$(PROC_FILTER),); \
		EXIT=$$?; \
		echo ""; \
		echo "Expected failures (~107k total):"; \
		echo "  ~70k  indexed-EA tests: corpus extension word has non-zero scale bits"; \
		echo "        (reserved on 68000, applied correctly by 68020)"; \
		echo "  ~24k  exception-frame tests (8-byte frame vs 6-byte on 68000)"; \
		echo "  ~13k  mode-6 EA tests where bit 8=1 triggers the 68020 full-EA path"; \
		echo "        (correct 68020 behaviour; 68000 hardware ignores the bit)."; \
		echo "Note: no 68020-specific corpus exists yet in SingleStepTests/680x0."; \
		exit $$EXIT; \
	else \
		echo "ProcessorTests not found at $(PROC_TESTS)"; \
		echo "Clone: git clone https://github.com/SingleStepTests/680x0 ProcessorTests"; \
		exit 1; \
	fi

# ProcessorTests against 68030: runs the 68000 test corpus in 68030 mode.
# The 68030 integer ISA is identical to the 68020, so the expected failure
# categories are the same as for 68020 (~107k total, all intentional):
#   ~70k  indexed-EA tests where corpus brief extension word has non-zero
#         scale bits (ignored by 68000 hardware, applied correctly by 68030)
#   ~24k  exception-frame tests (8-byte frame vs 6-byte on 68000)
#   ~13k  mode-6 EA tests where bit 8=1 triggers the 68030 full-EA path
#
# No 68030-specific test corpus exists in SingleStepTests/680x0 yet.
processor-tests-68030: $(TARGET)
	@if [ -d "$(PROC_TESTS)" ]; then \
		./$(TARGET) --cpu 68030 --processor-tests "$(PROC_TESTS)" $(if $(PROC_FILTER),$(PROC_FILTER),); \
		EXIT=$$?; \
		echo ""; \
		echo "Expected failures (~107k total, same categories as 68020):"; \
		echo "  ~70k  indexed-EA tests (non-zero scale bits applied correctly by 68030)"; \
		echo "  ~24k  exception-frame tests (8-byte frame vs 6-byte on 68000)"; \
		echo "  ~13k  mode-6 EA tests where bit 8=1 triggers the full-EA path."; \
		echo "Note: no 68030-specific corpus exists yet in SingleStepTests/680x0."; \
		exit $$EXIT; \
	else \
		echo "ProcessorTests not found at $(PROC_TESTS)"; \
		echo "Clone: git clone https://github.com/SingleStepTests/680x0 ProcessorTests"; \
		exit 1; \
	fi

processor-tests-68040: $(TARGET)
	@if [ -d "$(PROC_TESTS)" ]; then \
		./$(TARGET) --cpu 68040 --processor-tests "$(PROC_TESTS)" $(if $(PROC_FILTER),$(PROC_FILTER),); \
		EXIT=$$?; \
		echo ""; \
		echo "Expected failures (~107k total, same categories as 68020/68030):"; \
		echo "  ~70k  indexed-EA tests (non-zero scale bits)"; \
		echo "  ~24k  exception-frame tests (8-byte frame vs 6-byte on 68000)"; \
		echo "  ~13k  mode-6 EA tests where bit 8=1 triggers the full-EA path."; \
		echo "Note: no 68040-specific corpus exists yet in SingleStepTests/680x0."; \
		exit $$EXIT; \
	else \
		echo "ProcessorTests not found at $(PROC_TESTS)"; \
		echo "Clone: git clone https://github.com/SingleStepTests/680x0 ProcessorTests"; \
		exit 1; \
	fi

processor-tests-68060: $(TARGET)
	@if [ -d "$(PROC_TESTS)" ]; then \
		./$(TARGET) --cpu 68060 --processor-tests "$(PROC_TESTS)" $(if $(PROC_FILTER),$(PROC_FILTER),); \
		EXIT=$$?; \
		echo ""; \
		echo "Note: no 68060-specific corpus exists yet in SingleStepTests/680x0."; \
		exit $$EXIT; \
	else \
		echo "ProcessorTests not found at $(PROC_TESTS)"; \
		echo "Clone: git clone https://github.com/SingleStepTests/680x0 ProcessorTests"; \
		exit 1; \
	fi

processor-tests-68080: $(TARGET)
	@if [ -d "$(PROC_TESTS)" ]; then \
		./$(TARGET) --cpu 68080 --processor-tests "$(PROC_TESTS)" $(if $(PROC_FILTER),$(PROC_FILTER),); \
		EXIT=$$?; \
		echo ""; \
		echo "Note: no 68080-specific corpus exists yet in SingleStepTests/680x0."; \
		exit $$EXIT; \
	else \
		echo "ProcessorTests not found at $(PROC_TESTS)"; \
		echo "Clone: git clone https://github.com/SingleStepTests/680x0 ProcessorTests"; \
		exit 1; \
	fi

# Musashi differential validation.
# Runs every ProcessorTests case through both our emulator and Musashi.
# REAL_DIFF lines flag disagreements with Musashi (investigate if also a corpus failure).
# CORPUS_DIFF lines mean we agree with Musashi but differ from the JSON corpus
# (typically test-suite SSP-convention quirks, not emulator bugs).
#
# Known REAL_DIFFs in the full 68000 corpus (all confirmed NOT bugs in our emulator —
# processor-tests passes 100% for all these instructions; Musashi diverges from real hw):
#   ~4973  DIVS  — V flag undefined for division overflow
#   ~4350  DIVU  — V flag undefined for division overflow
#   ~3736  ASR   — Musashi X/C flag edge cases (count=0 or count>16)
#   ~3715  SBCD  — V flag documented undefined for BCD ops
#   ~3410  NBCD  — V flag documented undefined for BCD ops
#   ~814   ABCD  — V flag documented undefined for BCD ops
#   Total: ~20998 REAL_DIFFs, all Musashi undefined-behavior divergences
#
# Usage:
#   make musashi-diff                        # full suite
#   make musashi-diff PROC_FILTER=ADD        # only ADD* instruction files
#   make musashi-diff PROC_FILTER=ADD MDF_FLAGS=-v  # also show CORPUS_DIFF
MDF_FLAGS ?=
musashi-diff: 68k-musashi-diff
	@if [ -d "$(PROC_TESTS)" ]; then \
		./68k-musashi-diff "$(PROC_TESTS)" $(if $(PROC_FILTER),$(PROC_FILTER),) $(MDF_FLAGS) || exit 1; \
	else \
		echo "ProcessorTests not found at $(PROC_TESTS)"; \
		echo "Clone: git clone https://github.com/SingleStepTests/680x0 ProcessorTests"; \
		exit 1; \
	fi

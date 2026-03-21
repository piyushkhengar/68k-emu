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

# Core sources shared by all targets
SRCS = src/main.c src/core/cpu.c src/core/memory.c src/core/ea.c \
       src/isa/move.c src/isa/alu.c src/isa/branch.c src/isa/control.c \
       src/isa/immediate.c src/isa/logic.c src/isa/shift.c src/isa/bit.c \
       src/isa/movem.c src/isa/movep.c \
       src/genesis/bus.c src/genesis/vdp.c src/genesis/io.c src/genesis/z80.c \
       src/genesis/psg.c src/genesis/ym2612.c \
       src/genesis/genesis.c \
       src/tests.c src/timing.c src/timing_tests.c src/processor_tests.c \
       deps/cJSON/cJSON.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test mcl68-test genesis

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Genesis target: rebuild with SDL2 for graphical output
#   - genesis.c recompiled with -DHAVE_SDL2 (separate .o to avoid conflicts)
#   - renderer.c compiled with SDL2 headers
genesis: GENESIS_OBJS = $(filter-out src/genesis/genesis.o,$(OBJS)) \
                        src/genesis/genesis_sdl.o src/genesis/renderer.o src/genesis/audio.o
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

clean:
	rm -f $(OBJS) src/genesis/genesis_sdl.o src/genesis/renderer.o src/genesis/audio.o $(TARGET)

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

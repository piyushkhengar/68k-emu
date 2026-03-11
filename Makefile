CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11 -Isrc -Isrc/core -Isrc/isa -I.
TARGET = 68k-emu

# Optional: -DHAVE_ZLIB and -lz for .json.gz support
ZLIB_CFLAGS ?= $(shell pkg-config --cflags zlib 2>/dev/null || echo "")
ZLIB_LIBS   ?= $(shell pkg-config --libs zlib 2>/dev/null || echo "-lz")
ifeq ($(shell echo 'int main(){return 0;}' | $(CC) -x c - -lz -o /dev/null 2>/dev/null && echo ok),ok)
  CFLAGS += -DHAVE_ZLIB
  LDFLAGS += -lz
endif

SRCS = src/main.c src/core/cpu.c src/core/memory.c src/core/ea.c \
       src/isa/move.c src/isa/alu.c src/isa/branch.c src/isa/control.c \
       src/isa/immediate.c src/isa/logic.c src/isa/shift.c src/isa/bit.c \
       src/isa/movem.c src/isa/movep.c \
       src/tests.c src/timing.c src/processor_tests.c \
       deps/cJSON/cJSON.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# SPEED: run tests at given MHz (e.g. make test SPEED=7.09). Omit for hyperspeed.
SPEED ?=
SPEED_ARG = $(if $(SPEED), --speed $(SPEED),)

# Regression tests: run all built-in tests in one process
test: $(TARGET)
	@./$(TARGET) --run-all-tests$(SPEED_ARG) || exit 1

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

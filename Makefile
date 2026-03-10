CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11 -Isrc -Isrc/core -Isrc/isa
TARGET = 68k-emu

SRCS = src/main.c src/core/cpu.c src/core/memory.c src/core/ea.c \
       src/isa/move.c src/isa/alu.c src/isa/branch.c src/isa/control.c \
       src/isa/immediate.c src/isa/logic.c src/isa/shift.c src/tests.c src/timing.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

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

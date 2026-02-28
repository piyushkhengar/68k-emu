CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11
TARGET = 68k-emu

SRCS = main.c cpu.c memory.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

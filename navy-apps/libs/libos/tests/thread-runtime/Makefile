CC ?= cc

CPPFLAGS += -D__NAVY__ -I../../src -I../../../libc/include
CFLAGS += -std=c11 -Wall -Wextra -Werror -g

TARGET := test_thread_runtime
SOURCES := test_thread_runtime.c ../../src/thread_runtime.c

.PHONY: all test clean

all: $(TARGET)

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SOURCES) ../../src/thread_runtime.h ../../src/syscall.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	$(RM) $(TARGET)

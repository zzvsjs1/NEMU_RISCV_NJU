CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -g -I../../src

THREAD_TABLE_TEST := test_thread_table
THREAD_TABLE_SRCS := test_thread_table.c ../../src/thread-table.c
AUDIO_POLICY_TEST := test_audio_write_policy
AUDIO_POLICY_SRCS := test_audio_write_policy.c ../../../nanos-lite/src/audio_policy.c

.PHONY: all test clean

all: $(THREAD_TABLE_TEST) $(AUDIO_POLICY_TEST)

test: all
	./$(THREAD_TABLE_TEST)
	./$(AUDIO_POLICY_TEST)

$(THREAD_TABLE_TEST): $(THREAD_TABLE_SRCS) ../../src/thread-table.h
	$(CC) $(CFLAGS) -o $@ $(THREAD_TABLE_SRCS)

$(AUDIO_POLICY_TEST): $(AUDIO_POLICY_SRCS) ../../../nanos-lite/src/audio_policy.h
	$(CC) $(CFLAGS) -o $@ $(AUDIO_POLICY_SRCS)

clean:
	rm -f $(THREAD_TABLE_TEST) $(AUDIO_POLICY_TEST)

CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -g -I../..

TEST := test_audio_write
SRCS := test_audio_write.c ../../audio_write.c

.PHONY: test clean

test: $(TEST)
	./$(TEST)

$(TEST): $(SRCS) ../../audio_write.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TEST)

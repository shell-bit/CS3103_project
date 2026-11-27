CC = gcc
CFLAGS = -std=gnu11 -Wall -Wextra

.PHONY: all clean
all: minifs

minifs: problem2.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) minifs

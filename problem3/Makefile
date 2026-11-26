CC = gcc
CFLAGS = -std=c11 -Wall -Wextra

.PHONY: all clean test
all: problem3

problem3: problem3.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: problem3
	python3 test_q3.py ./problem3

clean:
	$(RM) problem3

# Blitter reference model — host build + unit tests (no hardware, no deps).
#   make test   build and run the unit tests
#   make clean
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror

test: test_blitter_ref
	./test_blitter_ref

test_blitter_ref: test_blitter_ref.c blitter_ref.c blitter_ref.h
	$(CC) $(CFLAGS) -o $@ test_blitter_ref.c blitter_ref.c

clean:
	rm -f test_blitter_ref

.PHONY: test clean

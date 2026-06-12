# Host emitter library + tests (no hardware, no deps).
#   make test    build and run the emitter/codec unit tests
#   make clean
CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -I../refmodel

test: test_emitter
	./test_emitter

test_emitter: test_emitter.c blt_emitter.c ../refmodel/blitter_ref.c \
              blt_emitter.h blt_wire.h ../refmodel/blitter_ref.h
	$(CC) $(CFLAGS) -o $@ test_emitter.c blt_emitter.c ../refmodel/blitter_ref.c

clean:
	rm -f test_emitter

.PHONY: test clean

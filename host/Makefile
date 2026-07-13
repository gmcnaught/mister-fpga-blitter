# Host emitter library + tests (no hardware, no deps).
#   make test    build and run the emitter/codec unit tests + embedded self-test
#   make clean
CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -I../refmodel

test: test_emitter selftest_emitter
	./test_emitter
	./selftest_emitter

# blitter_ref.c's blt_execute now dispatches BLT_OP_TRILIST to blt_raster_tri,
# so linking blitter_ref.c also requires ../refmodel/blt_tri.c.
test_emitter: test_emitter.c blt_emitter.c blt_alloc.c ../refmodel/blitter_ref.c ../refmodel/blt_tri.c \
              blt_emitter.h blt_wire.h blt_alloc.h ../refmodel/blitter_ref.h ../refmodel/blt_tri.h
	$(CC) $(CFLAGS) -o $@ test_emitter.c blt_emitter.c blt_alloc.c ../refmodel/blitter_ref.c ../refmodel/blt_tri.c

# Embedded self-test (STAGE / SDRAM staging / tile lists / colormod emit paths).
selftest_emitter: blt_emitter.c blt_alloc.c blt_emitter.h blt_wire.h blt_alloc.h \
                  ../refmodel/blitter_ref.h
	$(CC) $(CFLAGS) -DBLT_EMITTER_SELFTEST -o $@ blt_emitter.c blt_alloc.c

clean:
	rm -f test_emitter selftest_emitter

.PHONY: test clean

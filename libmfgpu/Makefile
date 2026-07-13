# libmfgpu — A9 geometry front-end (transform/cull -> BLT_OP_TRILIST display list).
#   make test    build and run the unit tests
#   make clean
CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -I../host -I../refmodel

# emitter + reference model this library builds on
DEPS   = ../host/blt_emitter.c ../host/blt_alloc.c \
         ../refmodel/blitter_ref.c ../refmodel/blt_tri.c
HDRS   = mfgpu.h ../host/blt_emitter.h ../host/blt_wire.h ../refmodel/blitter_ref.h

test: test_mfgpu
	./test_mfgpu

test_mfgpu: test_mfgpu.c mfgpu.c $(DEPS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_mfgpu.c mfgpu.c $(DEPS)

clean:
	rm -f test_mfgpu

.PHONY: test clean

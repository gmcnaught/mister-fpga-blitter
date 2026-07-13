# Blitter reference model — host build + unit tests (no hardware, no deps).
#   make test   build and run the unit tests + the embedded self-test
#   make clean
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror

test: test_blitter_ref selftest_ref test_blt_tri
	./test_blitter_ref
	./selftest_ref
	./test_blt_tri

# blitter_ref.c's blt_execute dispatches BLT_OP_TRILIST to blt_raster_tri, so
# every target that links blitter_ref.c must also link blt_tri.c.
test_blitter_ref: test_blitter_ref.c blitter_ref.c blt_tri.c blitter_ref.h blt_tri.h
	$(CC) $(CFLAGS) -o $@ test_blitter_ref.c blitter_ref.c blt_tri.c

# Textured-triangle rasterizer (BLT_OP_TRILIST) golden + tests.
test_blt_tri: test_blt_tri.c blt_tri.c blitter_ref.c blt_tri.h blitter_ref.h
	$(CC) $(CFLAGS) -o $@ test_blt_tri.c blt_tri.c blitter_ref.c

# Embedded self-test (v2 semantics: COLORMOD tint / ADD / MULTIPLY, TILELIST +
# TILELIST_RES vs N-blits equivalence) — exhaustive over the divide-free reductions.
selftest_ref: blitter_ref.c blt_tri.c blitter_ref.h blt_tri.h
	$(CC) $(CFLAGS) -DBLT_REF_SELFTEST -o $@ blitter_ref.c blt_tri.c

clean:
	rm -f test_blitter_ref selftest_ref test_blt_tri

.PHONY: test clean

# Blitter RTL simulation: diff blitter_top against the C reference model.
#   make test    build + run all scenarios (requires iverilog + cc)
#   make clean
SCEN = fill copy colorkey alpha hflip vflip clip_neg clip_off overdraw clear target1 \
       tri_copy tri_alpha tri_overlap tri_add tri_clip tri_rot

IVERILOG ?= iverilog
VVP      ?= vvp
CC       ?= cc

# C<->Verilog geometry roots must agree (native-288x216 single-source rule).
dims-check:
	@cw=$$(sed -n 's/#define BLT_FB_WIDTH[[:space:]]*\([0-9]*\).*/\1/p' ../refmodel/blitter_ref.h); \
	ch=$$(sed -n 's/#define BLT_FB_HEIGHT[[:space:]]*\([0-9]*\).*/\1/p' ../refmodel/blitter_ref.h); \
	vw=$$(sed -n 's/`define FB_W[[:space:]]*\([0-9]*\).*/\1/p' ../rtl/blitter_defs.vh); \
	vh=$$(sed -n 's/`define FB_H[[:space:]]*\([0-9]*\).*/\1/p' ../rtl/blitter_defs.vh); \
	if [ "$$cw" != "$$vw" ] || [ "$$ch" != "$$vh" ]; then \
	  echo "DIMS MISMATCH: blitter_ref.h $$cw x $$ch vs blitter_defs.vh $$vw x $$vh"; exit 1; \
	fi; echo "dims-check OK ($$cw x $$ch)"

test: dims-check gen_vectors blitter_tb.vvp
	@pass=0; fail=0; \
	for s in $(SCEN); do \
	  ./gen_vectors $$s >/dev/null; \
	  out=$$($(VVP) blitter_tb.vvp +name=$$s 2>&1); \
	  line=$$(echo "$$out" | grep RESULT); \
	  echo "$$out" | grep -v RESULT | sed 's/^/    /'; \
	  echo "$$line"; \
	  if echo "$$line" | grep -q PASS; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo "==================================="; \
	echo "  $$pass passed, $$fail failed"; \
	test $$fail -eq 0

blitter_tb.vvp: tb_blitter.sv ddr_model.sv ../rtl/blitter_top.sv ../rtl/blt_tri.sv ../rtl/blitter_defs.vh
	$(IVERILOG) -g2012 -I../rtl -o $@ tb_blitter.sv ddr_model.sv ../rtl/blitter_top.sv ../rtl/blt_tri.sv

gen_vectors: gen_vectors.c ../refmodel/blitter_ref.c ../refmodel/blt_tri.c ../refmodel/blitter_ref.h ../refmodel/blt_tri.h ../host/blt_wire.h
	$(CC) -O2 -Wall -I../refmodel -I../host -o $@ gen_vectors.c ../refmodel/blitter_ref.c ../refmodel/blt_tri.c

clean:
	rm -f gen_vectors blitter_tb.vvp ddr_init.hex fb_expected.hex

.PHONY: test clean dims-check

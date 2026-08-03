# Qt-front-end offload prototype — host build + tests (no hardware, no deps).
#   make test    build and run the offload gates against the reference model
#   make demo    composite the sample menu frames and write PPMs into out/
#   make clean
#
# The Qt paint-engine adapter (qt_blitter_paintengine.{h,cpp}) is deliberately
# NOT built here: this repo has no Qt dependency, and the adapter is a thin
# translation of QPainter calls into the ui_offload.c API that IS gated below.
# See README.md for the out-of-tree build recipe.
CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -I../../host -I../../refmodel

DEPS = ../../host/blt_emitter.c ../../host/blt_alloc.c \
       ../../refmodel/blitter_ref.c ../../refmodel/blt_tri.c
SRCS = ui_offload.c corner_atlas.c font6x8.c zaparoo_ui.c glyph_cache.c font_stroke.c
HDRS = ui_offload.h corner_atlas.h font6x8.h zaparoo_ui.h glyph_cache.h font_stroke.h \
       ../../host/blt_emitter.h ../../host/blt_wire.h ../../refmodel/blitter_ref.h

test: test_ui_offload
	./test_ui_offload

demo: demo_frame
	./demo_frame

test_ui_offload: test_ui_offload.c $(SRCS) $(DEPS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_ui_offload.c $(SRCS) $(DEPS) -lm

demo_frame: demo_frame.c $(SRCS) $(DEPS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ demo_frame.c $(SRCS) $(DEPS) -lm

clean:
	rm -f test_ui_offload demo_frame
	rm -rf out

.PHONY: test demo clean

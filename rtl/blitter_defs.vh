// Shared simulation memory layout (qword indices) for the blitter spike.
// MUST stay in sync with sim/gen_vectors.c (same constants, same names).
// These are SIM addresses; real f2h physical bases are wired at integration.
`ifndef BLITTER_DEFS_VH
`define BLITTER_DEFS_VH

// GEOMETRY ROOT (Verilog domain). MUST equal refmodel/blitter_ref.h
// BLT_FB_WIDTH/HEIGHT — enforced by `make -C ../sim dims-check`.
`define FB_W        288
`define FB_H        216
// Everything below DERIVES from the root — never retype a dimension.
`define FB_QWORDS   (`FB_W * `FB_H / 4)          // 15552 qwords (2 B/px, 8 B/qword)
`define FB_PIXELS   (`FB_W * `FB_H)              // 62208
`define FB_STRIDE_QW (`FB_W / 4)                 // 72 qwords per row

`define FB0_QW      32'd0
`define FB1_QW      (32'(`FB_QWORDS))            // second buffer right after the first
`define VCTRL_QW    (32'(2 * `FB_QWORDS))        // video control word (drop-in producer)
`define BLTCTRL_QW  (`VCTRL_QW + 32'd16)         // blitter control block base
`define RING_QW     (`VCTRL_QW + 32'd32)         // command ring base (4 qwords / command)
`define SRC_QW      32'd39000                    // source surface heap base (ring end 32160 < this)
`define MEM_QW      32'd65536                    // behavioral memory size (qwords)

// control-block field offsets (qwords from BLTCTRL_QW), low 32 bits used
`define C_SUBMIT    32'd0
`define C_CMDCOUNT  32'd1
`define C_TARGET    32'd2
`define C_CLEAR     32'd3
`define C_FLAGS     32'd4
`define C_DONE      32'd5
`define C_STATUS    32'd6

`endif

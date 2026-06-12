// Shared simulation memory layout (qword indices) for the blitter spike.
// MUST stay in sync with sim/gen_vectors.c (same constants, same names).
// These are SIM addresses; real f2h physical bases are wired at integration.
`ifndef BLITTER_DEFS_VH
`define BLITTER_DEFS_VH

`define FB_W        320
`define FB_H        240
`define FB_QWORDS   19200          // 320*240*2 / 8

`define FB0_QW      32'd0
`define FB1_QW      32'd19200
`define VCTRL_QW    32'd38400      // video control word (drop-in producer)
`define BLTCTRL_QW  32'd38416      // blitter control block base
`define RING_QW     32'd38432      // command ring base (4 qwords / command)
`define SRC_QW      32'd39000      // source surface heap base
`define MEM_QW      32'd65536      // behavioral memory size (qwords)

// control-block field offsets (qwords from BLTCTRL_QW), low 32 bits used
`define C_SUBMIT    32'd0
`define C_CMDCOUNT  32'd1
`define C_TARGET    32'd2
`define C_CLEAR     32'd3
`define C_FLAGS     32'd4
`define C_DONE      32'd5
`define C_STATUS    32'd6

`endif

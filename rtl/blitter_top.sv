// VENDORED from github.com/gmcnaught/mister-fpga-blitter (rtl/blitter_top.sv)
// HW addresses come from blitter_defs.vh (this dir). Do not edit here; edit upstream + re-copy.
//============================================================================
//  blitter_top.sv — MiSTer fabric 2D blitter: functional spike (#003)
//
//  Walks a DDR command ring (until END / cmd_count), composites into a
//  framebuffer in DDR per the host<->fabric contract (docs/blitter-protocol.md),
//  then writes the video control word as a DROP-IN PRODUCER for the existing
//  scanout reader. Verified bit-exact in simulation against the C reference
//  model (refmodel/blitter_ref.c) over the full v1 command set.
//
//  SCOPE NOTE: this spike uses a single Avalon-MM master with simple per-pixel
//  reads/writes (byte-enable lane writes) — deliberately FUNCTIONAL, not yet
//  bandwidth-optimal. The on-chip line/tile buffer + burst-DMA refinement (the
//  CV1000 pattern, see docs/) is the #004/#005 perf work and does not change
//  these command/handshake/pixel semantics.
//
//  Command word on-wire layout (32 bytes = 4 qwords, little-endian):
//    u32[0] = opcode[7:0] | blend[15:8] | format[23:16] | flags[31:24]
//    u32[1] = src_off[31:0]
//    u32[2] = src_stride[15:0] | src_x[31:16]
//    u32[3] = w[15:0] | h[31:16]
//    u32[4] = src_y[15:0] | resv
//    u32[5] = dst_x[15:0] | dst_y[31:16]   (signed16)
//    u32[6] = colorkey[15:0] | alpha[23:16] | priority[31:24]
//    u32[7] = color[15:0] | resv
//    qw_k = {u32[2k+1], u32[2k]}
//
//  Copyright (C) 2026 — GPL-3.0
//============================================================================
`default_nettype none
`include "blitter_defs.vh"

module blitter_top #(
    parameter AW = 32
) (
    input  wire          clk,
    input  wire          rst,
    // Avalon-MM-ish master to shared DDR (qword addressed)
    output reg  [AW-1:0] mem_addr,
    output reg           mem_rd,
    output reg           mem_wr,
    output reg  [63:0]   mem_din,
    output reg  [7:0]    mem_be,
    input  wire [63:0]   mem_dout,
    input  wire          mem_dout_ready,
    input  wire          mem_busy,    // reserved (sim model never busy)
    output reg           idle
);
    localparam [5:0]
        S_POLL_SUBMIT=6'd0, S_POLL_DONE=6'd1, S_CHK_NEW=6'd2,
        S_GOT_CMDCNT=6'd3,  S_GOT_TARGET=6'd4, S_GOT_FLAGS=6'd5, S_GOT_CLEAR=6'd6,
        S_CLR_WR=6'd7,      S_FETCH=6'd8,  S_COLLECT=6'd9, S_DECODE=6'd10,
        S_SETUP=6'd11,      S_FILL_WR=6'd12,
        S_BLIT_RDSRC=6'd13, S_BLIT_GOTSRC=6'd14, S_BLIT_RDDST=6'd15,
        S_BLIT_GOTDST=6'd16, S_BLIT_WR=6'd17, S_PIX_ADV=6'd18, S_NEXT_CMD=6'd19,
        S_FRAME_VCTRL=6'd20, S_WR_DONE=6'd21, S_WR_STATUS=6'd22,
        S_RD_WAIT=6'd23,    S_WR_WAIT=6'd24,
        S_BSETUP=6'd25,     // isolated source-base multiply (timing)
        S_BLIT_BLEND2=6'd26,// 2nd blend stage: /255 reduce + RGB565 pack (timing)
        // [MFGPU] BLT_OP_TRILIST states
        S_TRI_VFETCH=6'd27, S_TRI_VCOLLECT=6'd28, S_TRI_DECV=6'd29,
        S_TRI_SETUP=6'd30,  S_TRI_PIX=6'd31, S_TRI_GOTTEX=6'd32,
        S_TRI_GOTDST=6'd33, S_TRI_WR=6'd34, S_TRI_ADV=6'd35;

    localparam [7:0] OP_NOP=8'd0, OP_END=8'd1, OP_FILL=8'd2, OP_BLIT=8'd3, OP_TRILIST=8'd8;
    localparam [7:0] BLEND_KEY=8'd1, BLEND_ALPHA=8'd2, BLEND_PALPHA=8'd3;
    localparam [7:0] F_HFLIP=8'h01, F_VFLIP=8'h02, F_COLORKEY=8'h04;
    // Source pixel formats (cmd.format). RGB565 keeps the v1 16bpp addressing;
    // ARGB4444 is also 16bpp ({A4,R4,G4,B4}) so src_byte_cur / +/-2 / src_sh are
    // UNCHANGED — BLEND_PALPHA just reinterprets the fetched 16-bit source pixel.
    localparam [7:0] FMT_RGB565=8'd0, FMT_ARGB4444=8'd1;

    reg  [5:0]  state, rd_ret, wr_ret;
    reg         rd_issued;   // read accepted by the bus, now awaiting dout_ready
    reg  [63:0] rd_data;

    reg  [31:0] submit_reg, done_reg, cmd_count, cmd_idx, frame_counter;
    reg         target_buf;
    reg  [31:0] target_base, cfg_flags, clr_idx;
    reg  [15:0] clear_color;
    reg  [63:0] cmd_qw [0:3];
    reg  [1:0]  fetch_k;

    reg  [7:0]  c_opcode, c_blend, c_format, c_flags, c_alpha;
    reg  [31:0] c_src_off;
    reg  [15:0] c_src_stride, c_src_x, c_src_y, c_w, c_h, c_colorkey, c_color;
    reg  signed [15:0] c_dst_x, c_dst_y;

    reg  signed [31:0] x0r, y0r, x1r, y1r, dx, dy;
    reg         is_fill;
    reg  [15:0] src_pix, wr_pix;
    reg  [31:0] src_byte_cur, src_row_byte;   // incremental source addressing
    reg  [15:0] src_x0s, src_y0s;             // source start coords (latched at S_SETUP)

    wire keyed = (c_blend == BLEND_KEY) || ((c_flags & F_COLORKEY) != 0);

    // ---- 2-STAGE BLEND (timing): the source-over composite is split across two
    // FSM cycles so no single clock does (multiply + /255 reduction + RGB565 pack)
    // feeding wr_pix. Both const-alpha (blend565) and per-pixel-alpha (blend4444)
    // are unified through the SAME weighted-sum -> reduce/pack datapath:
    //   Stage 1 (S_BLIT_GOTDST): extract per-channel src (5/6/5) + 8-bit alpha
    //     (RGB565 src direct + const c_alpha; ARGB4444 src expands 4->5/6/5 and
    //     a8={a4,a4}), read dst channels, and compute & REGISTER the weighted
    //     sums tr=sr*a8 + dr*na, tg, tb.
    //   Stage 2 (S_BLIT_BLEND2): the divide-free /255 reduction
    //     (t+128+((t+128)>>8))>>8 per channel + the RGB565 pack into wr_pix.
    // Bit-exact to the previous single-cycle blend565/blend4444 (just 1 cycle
    // later); proven by tb_blitter_blend (CONST_ALPHA) + tb_blitter_palpha.
    //
    // Stage-1 outputs (registered): weighted per-channel sums. Max value is
    // 63*255*2 = 32130 (16 bits suffice, but tr+128 needs the headroom -> 17b).
    reg [16:0] blend_tr, blend_tg, blend_tb;

    // Stage-1 combinational channel/alpha extraction (off src_pix / dst_pix_w):
    //  - alpha a8: const path = c_alpha; per-pixel = {a4,a4}
    //  - src channels expanded to dst widths (5/6/5)
    wire        b_palpha = (c_blend == BLEND_PALPHA);
    wire [7:0]  b_a8  = b_palpha ? {src_pix[15:12], src_pix[15:12]} : c_alpha;
    wire [7:0]  b_na  = 8'd255 - b_a8;
    wire [4:0]  b_sr  = b_palpha ? {src_pix[11:8],  src_pix[11]}   : src_pix[15:11];
    wire [5:0]  b_sg  = b_palpha ? {src_pix[7:4],   src_pix[7:6]}  : src_pix[10:5];
    wire [4:0]  b_sb  = b_palpha ? {src_pix[3:0],   src_pix[3]}    : src_pix[4:0];
    // b_dr/b_dg/b_db (dst channels) declared after dst_pix_w below.

    // Stage-2 divide-free /255 reduction (same form as the old refmodel):
    //   o = (t + 128 + ((t+128)>>8)) >> 8
    function [5:0] reduce255(input [16:0] t);
        reg [17:0] t128;
        begin
            t128 = {1'b0, t} + 18'd128;
            reduce255 = (t128 + (t128 >> 8)) >> 8;
        end
    endfunction
    wire [5:0] blend_or = reduce255(blend_tr);
    wire [5:0] blend_og = reduce255(blend_tg);
    wire [5:0] blend_ob = reduce255(blend_tb);

    // ---- clip (combinational off decoded c_*) --------------------------
    wire signed [31:0] sdx = c_dst_x, sdy = c_dst_y;
    wire signed [31:0] xe = sdx + c_w, ye = sdy + c_h;
    wire signed [31:0] clip_x0 = (sdx<0)?0:sdx;
    wire signed [31:0] clip_y0 = (sdy<0)?0:sdy;
    wire signed [31:0] clip_x1 = (xe>`FB_W)?`FB_W:xe;
    wire signed [31:0] clip_y1 = (ye>`FB_H)?`FB_H:ye;
    wire empty = (clip_x0>=clip_x1) || (clip_y0>=clip_y1);

    // ---- source addressing: REGISTERED INCREMENTAL --------------------------
    // Was a per-pixel 16x16 multiply (c_src_y+sy)*c_src_stride sitting on the
    // dy -> wr_pix critical path (setup slack -7.348 ns). src_byte_cur is now
    // maintained by adds in S_PIX_ADV (+/-2 per pixel, +/-stride per row); the
    // single multiply is isolated once-per-blit in S_BSETUP.
    wire [31:0] src_qw    = `SRC_QW + (src_byte_cur >> 3);
    wire [5:0]  src_sh    = {src_byte_cur[2:1], 4'b0};
    wire [15:0] src_pix_w = rd_data[src_sh +: 16];   // registered DDR data (see rd_data)
    // signed source-local start coords at the clipped origin (off c_*, dst)
    wire signed [31:0] sx0 = clip_x0 - sdx;   // = lx at (clip_x0)
    wire signed [31:0] sy0 = clip_y0 - sdy;   // = ly at (clip_y0)

    // ---- dest addressing: REGISTERED INCREMENTAL (timing) -------------------
    // dst_pidx = dy*320 + dx was a per-pixel 16x16 multiply feeding the dst_qw /
    // dst_sh / lane_be / dst_pix_w chain on the dx -> wr_pix critical path. Like
    // the source cursor, it is now maintained by adds: +1 per pixel in a row, and
    // reset to the row start (+320) per row in S_PIX_ADV. The single base
    // multiply (y0*320 + x0) is isolated once-per-blit in S_BSETUP / S_FILL setup.
    reg  [31:0] dst_pidx_r, dst_row_pidx_r;
    wire [31:0] dst_pidx = dst_pidx_r;
    wire [31:0] dst_qw   = target_base + (dst_pidx >> 2);
    wire [5:0]  dst_sh   = {dst_pidx[1:0], 4'b0};
    wire [7:0]  lane_be  = 8'h03 << {dst_pidx[1:0], 1'b0};
    wire [15:0] dst_pix_w = rd_data[dst_sh +: 16];   // registered DDR data (see rd_data)
    // base index at the clipped origin: dy*320 = (dy<<8)+(dy<<6) shift-add.
    wire [31:0] dst_base_pidx = (clip_y0<<8) + (clip_y0<<6) + clip_x0;
    // stage-1 dst channel extraction (uses dst_pix_w declared just above)
    wire [4:0]  b_dr  = dst_pix_w[15:11];
    wire [5:0]  b_dg  = dst_pix_w[10:5];
    wire [4:0]  b_db  = dst_pix_w[4:0];

    // video control word (drop-in producer): frame_counter[31:2] | buf[1:0]
    wire [31:0] vctrl_val = ((frame_counter + 32'd1) << 2) | {31'd0, target_buf};

    // ==== [MFGPU] BLT_OP_TRILIST ============================================
    reg  [15:0] tri_idx, tri_count;
    reg  [31:0] entry_qw_base;                 // SRC_QW + entry_off/8
    reg  [2:0]  vfetch_k;
    reg  [63:0] vqw [0:5];                      // 3 verts x 2 qwords
    reg  signed [15:0] tvx0,tvy0, tvx1,tvy1, tvx2,tvy2;
    reg  [15:0] tvu0,tvv0, tvu1,tvv1, tvu2,tvv2;
    reg  [7:0]  tvr0,tvg0,tvb0,tva0, tvr1,tvg1,tvb1,tva1, tvr2,tvg2,tvb2,tva2;
    reg  [15:0] txmin,txmax,tymin,tymax, tpx,tpy;
    reg  [15:0] tri_texel, tri_dst;

    wire        tri_hit, tri_we;
    wire [15:0] tri_tu, tri_tv, tri_out;
    wire [7:0]  tri_cr, tri_cg, tri_cb, tri_ca;
    blt_tri u_tri (
        .vx0(tvx0),.vy0(tvy0),.vx1(tvx1),.vy1(tvy1),.vx2(tvx2),.vy2(tvy2),
        .vu0(tvu0),.vv0(tvv0),.vu1(tvu1),.vv1(tvv1),.vu2(tvu2),.vv2(tvv2),
        .vr0(tvr0),.vg0(tvg0),.vb0(tvb0),.va0(tva0),
        .vr1(tvr1),.vg1(tvg1),.vb1(tvb1),.va1(tva1),
        .vr2(tvr2),.vg2(tvg2),.vb2(tvb2),.va2(tva2),
        .tex_w(c_src_x),.tex_h(c_src_y), .px(tpx),.py(tpy),
        .hit(tri_hit),.tu(tri_tu),.tv(tri_tv),
        .cr(tri_cr),.cg(tri_cg),.cb(tri_cb),.ca(tri_ca),
        .texel(tri_texel),.dst(tri_dst),
        .g_alpha(c_alpha),.blend_mode(c_blend),.colorkey(c_colorkey),
        .write_en(tri_we),.out_pix(tri_out)
    );
    // texel address (bytes -> qword + half-word lane), src heap base at SRC_QW
    wire [31:0] tex_byte = c_src_off + tri_tv*c_src_stride + {15'd0, tri_tu, 1'b0};
    wire [31:0] tex_qw   = `SRC_QW + (tex_byte >> 3);
    wire [5:0]  tex_sh   = {tex_byte[2:1], 4'b0};
    // TRILIST dst address (tpy*320 + tpx)
    wire [31:0] tri_dpidx = tpy*`FB_W + tpx;
    wire [31:0] tri_dqw   = target_base + (tri_dpidx >> 2);
    wire [5:0]  tri_dsh   = {tri_dpidx[1:0], 4'b0};
    wire [7:0]  tri_dbe   = 8'h03 << {tri_dpidx[1:0], 1'b0};
    // bounding box: min/max of the 3 signed vertex coords, >>4, clamped to FB
    wire signed [15:0] tvminx = (tvx0<tvx1)?((tvx0<tvx2)?tvx0:tvx2):((tvx1<tvx2)?tvx1:tvx2);
    wire signed [15:0] tvmaxx = (tvx0>tvx1)?((tvx0>tvx2)?tvx0:tvx2):((tvx1>tvx2)?tvx1:tvx2);
    wire signed [15:0] tvminy = (tvy0<tvy1)?((tvy0<tvy2)?tvy0:tvy2):((tvy1<tvy2)?tvy1:tvy2);
    wire signed [15:0] tvmaxy = (tvy0>tvy1)?((tvy0>tvy2)?tvy0:tvy2):((tvy1>tvy2)?tvy1:tvy2);
    wire signed [31:0] bb_minx = $signed(tvminx) >>> 4;
    wire signed [31:0] bb_maxx = ($signed(tvmaxx) + 32'sd15) >>> 4;
    wire signed [31:0] bb_miny = $signed(tvminy) >>> 4;
    wire signed [31:0] bb_maxy = ($signed(tvmaxy) + 32'sd15) >>> 4;
    wire signed [31:0] cl_minx = (bb_minx < 0) ? 32'sd0 : bb_minx;
    wire signed [31:0] cl_maxx = (bb_maxx > (`FB_W-1)) ? (`FB_W-1) : bb_maxx;
    wire signed [31:0] cl_miny = (bb_miny < 0) ? 32'sd0 : bb_miny;
    wire signed [31:0] cl_maxy = (bb_maxy > (`FB_H-1)) ? (`FB_H-1) : bb_maxy;
    wire tri_empty = (cl_minx>cl_maxx)||(cl_miny>cl_maxy)||(cl_maxx<0)||(cl_maxy<0);

    always @(posedge clk) begin
        if (rst) begin
            state<=S_POLL_SUBMIT; mem_rd<=0; mem_wr<=0; mem_be<=0;
            mem_addr<=0; mem_din<=0; idle<=1; frame_counter<=0;
            cmd_idx<=0; fetch_k<=0; submit_reg<=0; done_reg<=0; rd_issued<=0;
        end else begin
            mem_rd<=1'b0;
            case (state)
            S_POLL_SUBMIT: begin
                idle<=1; mem_rd<=1; mem_addr<=`BLTCTRL_QW+`C_SUBMIT;
                rd_ret<=S_POLL_DONE; state<=S_RD_WAIT;
            end
            S_POLL_DONE: begin
                submit_reg<=rd_data[31:0];
                mem_rd<=1; mem_addr<=`BLTCTRL_QW+`C_DONE;
                rd_ret<=S_CHK_NEW; state<=S_RD_WAIT;
            end
            S_CHK_NEW: begin
                done_reg<=rd_data[31:0];
                if (rd_data[31:0]==submit_reg) state<=S_POLL_SUBMIT;   // idle: keep polling
                else begin
                    idle<=0; mem_rd<=1; mem_addr<=`BLTCTRL_QW+`C_CMDCOUNT;
                    rd_ret<=S_GOT_CMDCNT; state<=S_RD_WAIT;
                end
            end
            S_GOT_CMDCNT: begin
                cmd_count<=rd_data[31:0];
                mem_rd<=1; mem_addr<=`BLTCTRL_QW+`C_TARGET;
                rd_ret<=S_GOT_TARGET; state<=S_RD_WAIT;
            end
            S_GOT_TARGET: begin
                target_buf<=rd_data[0];
                target_base<=rd_data[0]?`FB1_QW:`FB0_QW;
                mem_rd<=1; mem_addr<=`BLTCTRL_QW+`C_FLAGS;
                rd_ret<=S_GOT_FLAGS; state<=S_RD_WAIT;
            end
            S_GOT_FLAGS: begin
                cfg_flags<=rd_data[31:0];
                mem_rd<=1; mem_addr<=`BLTCTRL_QW+`C_CLEAR;
                rd_ret<=S_GOT_CLEAR; state<=S_RD_WAIT;
            end
            S_GOT_CLEAR: begin
                clear_color<=rd_data[15:0];
                if (cfg_flags[0]) begin clr_idx<=0; state<=S_CLR_WR; end
                else begin cmd_idx<=0; fetch_k<=0; state<=S_FETCH; end
            end
            S_CLR_WR: begin
                if (clr_idx==`FB_QWORDS) begin
                    cmd_idx<=0; fetch_k<=0; state<=S_FETCH;
                end else begin
                    mem_wr<=1; mem_be<=8'hFF; mem_addr<=target_base+clr_idx;
                    mem_din<={4{clear_color}}; clr_idx<=clr_idx+1;
                    wr_ret<=S_CLR_WR; state<=S_WR_WAIT;
                end
            end

            S_FETCH: begin
                if (cmd_idx>=cmd_count) state<=S_FRAME_VCTRL;
                else begin
                    fetch_k<=0; mem_rd<=1; mem_addr<=`RING_QW+cmd_idx*4;
                    rd_ret<=S_COLLECT; state<=S_RD_WAIT;
                end
            end
            S_COLLECT: begin
                cmd_qw[fetch_k]<=rd_data;
                if (fetch_k==2'd3) state<=S_DECODE;
                else begin
                    mem_rd<=1; mem_addr<=`RING_QW+cmd_idx*4+(fetch_k+2'd1);
                    fetch_k<=fetch_k+2'd1; rd_ret<=S_COLLECT; state<=S_RD_WAIT;
                end
            end
            S_DECODE: begin
                c_opcode    <= cmd_qw[0][7:0];
                c_blend     <= cmd_qw[0][15:8];
                c_format    <= cmd_qw[0][23:16];
                c_flags     <= cmd_qw[0][31:24];
                c_src_off   <= cmd_qw[0][63:32];
                c_src_stride<= cmd_qw[1][15:0];
                c_src_x     <= cmd_qw[1][31:16];
                c_w         <= cmd_qw[1][47:32];
                c_h         <= cmd_qw[1][63:48];
                c_src_y     <= cmd_qw[2][15:0];
                c_dst_x     <= cmd_qw[2][47:32];
                c_dst_y     <= cmd_qw[2][63:48];
                c_colorkey  <= cmd_qw[3][15:0];
                c_alpha     <= cmd_qw[3][23:16];
                c_color     <= cmd_qw[3][47:32];
                state<=S_SETUP;
            end
            S_SETUP: begin
                if (c_opcode==OP_END)       state<=S_FRAME_VCTRL;
                else if (c_opcode==OP_NOP)  state<=S_NEXT_CMD;
                else if (c_opcode==OP_TRILIST) begin
                    // [MFGPU] TRILIST: dst_x|dst_y<<16 = vertex entry byte offset,
                    // w = triangle count. (The rect-clip `empty` is meaningless here.)
                    tri_count     <= c_w;
                    entry_qw_base <= `SRC_QW + ({c_dst_y, c_dst_x} >> 3);
                    tri_idx       <= 16'd0;
                    state         <= S_TRI_VFETCH;
                end
                else if (empty)             state<=S_NEXT_CMD;
                else begin
                    x0r<=clip_x0; y0r<=clip_y0; x1r<=clip_x1; y1r<=clip_y1;
                    dx<=clip_x0;  dy<=clip_y0; is_fill<=(c_opcode==OP_FILL);
                    // dst write-index base (shift-add multiply), once per blit;
                    // per-pixel/per-row it is maintained by adds in S_PIX_ADV.
                    dst_pidx_r     <= dst_base_pidx;
                    dst_row_pidx_r <= dst_base_pidx;
                    // latch source-local start coords (flip-aware); the base
                    // multiply happens once in S_BSETUP (off the per-pixel path)
                    src_x0s <= c_src_x + ((c_flags&F_HFLIP) ? (c_w-1 - sx0[15:0]) : sx0[15:0]);
                    src_y0s <= c_src_y + ((c_flags&F_VFLIP) ? (c_h-1 - sy0[15:0]) : sy0[15:0]);
                    state<=(c_opcode==OP_FILL)?S_FILL_WR:S_BSETUP;
                end
            end
            S_BSETUP: begin
                // single isolated source-base multiply (src_y*stride); per-pixel
                // addressing is pure adds from here on
                src_row_byte <= c_src_off + src_y0s*c_src_stride + {15'd0, src_x0s, 1'b0};
                src_byte_cur <= c_src_off + src_y0s*c_src_stride + {15'd0, src_x0s, 1'b0};
                state<=S_BLIT_RDSRC;
            end

            S_FILL_WR: begin
                mem_wr<=1; mem_be<=lane_be; mem_addr<=dst_qw;
                mem_din<=({48'd0,c_color}<<dst_sh);
                wr_ret<=S_PIX_ADV; state<=S_WR_WAIT;
            end
            S_BLIT_RDSRC: begin
                mem_rd<=1; mem_addr<=src_qw; rd_ret<=S_BLIT_GOTSRC; state<=S_RD_WAIT;
            end
            S_BLIT_GOTSRC: begin
                src_pix<=src_pix_w;
                if (keyed && (src_pix_w==c_colorkey)) state<=S_PIX_ADV; // skip-write
                // per-pixel alpha: fully-transparent source (A4==0) -> skip-write
                else if (c_blend==BLEND_PALPHA && (src_pix_w[15:12]==4'd0))
                    state<=S_PIX_ADV;
                else if (c_blend==BLEND_ALPHA || c_blend==BLEND_PALPHA) begin
                    mem_rd<=1; mem_addr<=dst_qw; rd_ret<=S_BLIT_GOTDST; state<=S_RD_WAIT;
                end else begin wr_pix<=src_pix_w; state<=S_BLIT_WR; end
            end
            // Stage 1: per-channel weighted sums (multiplies) -> registers.
            // src alpha/channel extraction (RGB565 vs ARGB4444) is the b_* wires.
            S_BLIT_GOTDST: begin
                blend_tr <= b_sr*b_a8 + b_dr*b_na;
                blend_tg <= b_sg*b_a8 + b_dg*b_na;
                blend_tb <= b_sb*b_a8 + b_db*b_na;
                state<=S_BLIT_BLEND2;
            end
            // Stage 2: /255 reduction + RGB565 pack (no multiply on this path).
            S_BLIT_BLEND2: begin
                wr_pix<={ blend_or[4:0], blend_og[5:0], blend_ob[4:0] };
                state<=S_BLIT_WR;
            end
            S_BLIT_WR: begin
                mem_wr<=1; mem_be<=lane_be; mem_addr<=dst_qw;
                mem_din<=({48'd0,wr_pix}<<dst_sh);
                wr_ret<=S_PIX_ADV; state<=S_WR_WAIT;
            end
            S_PIX_ADV: begin
                if ((dx+1)>=x1r) begin
                    dx<=x0r;
                    if ((dy+1)>=y1r) state<=S_NEXT_CMD;
                    else begin
                        dy<=dy+1;
                        // next row: dst index steps to the next row start (+320),
                        // dst cursor reset to it (mirrors src_byte_cur reset).
                        dst_row_pidx_r <= dst_row_pidx_r + `FB_W;
                        dst_pidx_r     <= dst_row_pidx_r + `FB_W;
                        // next row: source y steps by +/-1 -> +/- stride bytes;
                        // reset the column cursor to the new row's start
                        src_row_byte <= (c_flags&F_VFLIP) ? src_row_byte - {16'd0,c_src_stride}
                                                          : src_row_byte + {16'd0,c_src_stride};
                        src_byte_cur <= (c_flags&F_VFLIP) ? src_row_byte - {16'd0,c_src_stride}
                                                          : src_row_byte + {16'd0,c_src_stride};
                        state<=is_fill?S_FILL_WR:S_BLIT_RDSRC;
                    end
                end else begin
                    dx<=dx+1;
                    // next pixel in row: dst index +1
                    dst_pidx_r <= dst_pidx_r + 32'd1;
                    // next pixel in row: source x steps by +/-1 -> +/-2 bytes
                    src_byte_cur <= (c_flags&F_HFLIP) ? src_byte_cur - 32'd2
                                                      : src_byte_cur + 32'd2;
                    state<=is_fill?S_FILL_WR:S_BLIT_RDSRC;
                end
            end
            // ==== [MFGPU] BLT_OP_TRILIST — vertex fetch, raster, blend, write ====
            S_TRI_VFETCH: begin
                if (tri_idx >= tri_count) state<=S_NEXT_CMD;
                else begin
                    vfetch_k<=3'd0; mem_rd<=1; mem_addr<=entry_qw_base + tri_idx*6;
                    rd_ret<=S_TRI_VCOLLECT; state<=S_RD_WAIT;
                end
            end
            S_TRI_VCOLLECT: begin
                vqw[vfetch_k]<=rd_data;
                if (vfetch_k==3'd5) state<=S_TRI_DECV;
                else begin
                    mem_rd<=1; mem_addr<=entry_qw_base + tri_idx*6 + vfetch_k + 3'd1;
                    vfetch_k<=vfetch_k+3'd1; rd_ret<=S_TRI_VCOLLECT; state<=S_RD_WAIT;
                end
            end
            S_TRI_DECV: begin
                // vertex qword layout (LE): qw0 = {v,u,y,x}; qw1[31:0] = rgba (r|g<<8|b<<16|a<<24)
                tvx0<=vqw[0][15:0]; tvy0<=vqw[0][31:16]; tvu0<=vqw[0][47:32]; tvv0<=vqw[0][63:48];
                tvr0<=vqw[1][7:0];  tvg0<=vqw[1][15:8];  tvb0<=vqw[1][23:16]; tva0<=vqw[1][31:24];
                tvx1<=vqw[2][15:0]; tvy1<=vqw[2][31:16]; tvu1<=vqw[2][47:32]; tvv1<=vqw[2][63:48];
                tvr1<=vqw[3][7:0];  tvg1<=vqw[3][15:8];  tvb1<=vqw[3][23:16]; tva1<=vqw[3][31:24];
                tvx2<=vqw[4][15:0]; tvy2<=vqw[4][31:16]; tvu2<=vqw[4][47:32]; tvv2<=vqw[4][63:48];
                tvr2<=vqw[5][7:0];  tvg2<=vqw[5][15:8];  tvb2<=vqw[5][23:16]; tva2<=vqw[5][31:24];
                state<=S_TRI_SETUP;
            end
            S_TRI_SETUP: begin
                if (tri_empty) begin tri_idx<=tri_idx+16'd1; state<=S_TRI_VFETCH; end
                else begin
                    txmin<=cl_minx[15:0]; txmax<=cl_maxx[15:0];
                    tymin<=cl_miny[15:0]; tymax<=cl_maxy[15:0];
                    tpx<=cl_minx[15:0];   tpy<=cl_miny[15:0];
                    state<=S_TRI_PIX;
                end
            end
            S_TRI_PIX: begin
                if (!tri_hit) state<=S_TRI_ADV;   // pixel center outside the triangle
                else begin mem_rd<=1; mem_addr<=tex_qw; rd_ret<=S_TRI_GOTTEX; state<=S_RD_WAIT; end
            end
            S_TRI_GOTTEX: begin
                tri_texel <= rd_data[tex_sh +: 16];
                // CONST_ALPHA(2)/ADD(4)/MULTIPLY(5) need the current dst pixel; COPY/KEY don't
                if ((c_blend==BLEND_ALPHA)||(c_blend==8'd4)||(c_blend==8'd5)) begin
                    mem_rd<=1; mem_addr<=tri_dqw; rd_ret<=S_TRI_GOTDST; state<=S_RD_WAIT;
                end else state<=S_TRI_WR;
            end
            S_TRI_GOTDST: begin
                tri_dst <= rd_data[tri_dsh +: 16];
                state<=S_TRI_WR;
            end
            S_TRI_WR: begin
                if (tri_we) begin
                    mem_wr<=1; mem_be<=tri_dbe; mem_addr<=tri_dqw;
                    mem_din<=({48'd0, tri_out} << tri_dsh);
                    wr_ret<=S_TRI_ADV; state<=S_WR_WAIT;
                end else state<=S_TRI_ADV;
            end
            S_TRI_ADV: begin
                if (tpx >= txmax) begin
                    tpx<=txmin;
                    if (tpy >= tymax) begin tri_idx<=tri_idx+16'd1; state<=S_TRI_VFETCH; end
                    else begin tpy<=tpy+16'd1; state<=S_TRI_PIX; end
                end else begin tpx<=tpx+16'd1; state<=S_TRI_PIX; end
            end

            S_NEXT_CMD: begin cmd_idx<=cmd_idx+1; state<=S_FETCH; end

            S_FRAME_VCTRL: begin
                mem_wr<=1; mem_be<=8'h0F; mem_addr<=`VCTRL_QW;
                mem_din<={32'd0, vctrl_val};
                frame_counter<=frame_counter+1;
                wr_ret<=S_WR_DONE; state<=S_WR_WAIT;
            end
            S_WR_DONE: begin
                mem_wr<=1; mem_be<=8'h0F; mem_addr<=`BLTCTRL_QW+`C_DONE;
                mem_din<={32'd0, submit_reg};
                wr_ret<=S_WR_STATUS; state<=S_WR_WAIT;
            end
            S_WR_STATUS: begin
                mem_wr<=1; mem_be<=8'h0F; mem_addr<=`BLTCTRL_QW+`C_STATUS;
                mem_din<=64'd0; wr_ret<=S_POLL_SUBMIT; state<=S_WR_WAIT;
            end

            // Backpressure-safe generic read: hold mem_rd until the bus accepts
            // it (~mem_busy), then await dout_ready. (mem_busy = ddram busy OR not
            // granted by the arbiter; on the never-busy sim model this is a no-op.)
            S_RD_WAIT: begin
                if (!rd_issued) begin
                    mem_rd <= 1'b1;                       // hold request
                    if (!mem_busy) rd_issued <= 1'b1;     // accepted this cycle
                end else if (mem_dout_ready) begin
                    rd_data <= mem_dout; rd_issued <= 1'b0; state <= rd_ret;
                end
            end
            // Backpressure-safe generic write: mem_wr/addr/din/be held from the
            // issue state; clear + advance only once the bus accepts (~mem_busy).
            S_WR_WAIT: if (!mem_busy) begin
                mem_wr <= 1'b0; mem_be <= 8'h00; state <= wr_ret;
            end
            default: state<=S_POLL_SUBMIT;
            endcase
        end
    end
endmodule
`default_nettype wire

//  blt_tri.sv — combinational per-pixel evaluator for BLT_OP_TRILIST.
//
//  Behavioral (longint) match to refmodel/blt_tri.c (blt_raster_tri). The FSM in
//  blitter_top registers the current triangle's 3 vertices and drives (px,py);
//  this module reports, combinationally:
//    - inside          : is the pixel center covered (top-left rule)
//    - tu,tv           : clamped nearest-texel coordinates (for the FSM's read)
//    - cr,cg,cb,ca     : interpolated per-vertex colour at the pixel
//  Then, once the FSM has fetched the texel (and dst, for blend modes that need
//  it), the module reports:
//    - write_en        : 1 unless COLORKEY culls this texel
//    - out_pix         : the final RGB565 to store
//
//  Bit-exact to the C by construction: same integer edge functions in 12.4, same
//  round-to-nearest divide (divr), same top-left bias, same nearest-texel clamp,
//  and the same divide-free /255, /31, /63 reductions used by the RGB565 helpers.
//  This is a simulation-equivalence model, not a synthesis-optimized datapath.
//
//  Copyright (C) 2026 — GPL-3.0.
`default_nettype none
module blt_tri (
    // raw triangle vertices (screen 12.4 signed), three verts a,b,c
    input  wire signed [15:0] vx0, vy0, vx1, vy1, vx2, vy2,
    input  wire        [15:0] vu0, vv0, vu1, vv1, vu2, vv2,   // texel 12.4 unsigned
    input  wire        [7:0]  vr0, vg0, vb0, va0,
    input  wire        [7:0]  vr1, vg1, vb1, va1,
    input  wire        [7:0]  vr2, vg2, vb2, va2,
    input  wire        [15:0] tex_w, tex_h,                   // texture size (texels)
    input  wire        [15:0] px, py,                         // sample pixel (>=0)
    // cover / interpolation outputs
    output reg                hit,       // pixel covered ('inside' is an SV keyword)
    output reg         [15:0] tu, tv,
    output reg         [7:0]  cr, cg, cb, ca,
    // blend inputs (driven after the FSM's texel/dst reads)
    input  wire        [15:0] texel, dst,
    input  wire        [7:0]  g_alpha,
    input  wire        [7:0]  blend_mode,
    input  wire        [15:0] colorkey,
    // blend outputs
    output reg                write_en,
    output reg         [15:0] out_pix
);
    localparam [7:0] BM_COPY=8'd0, BM_KEY=8'd1, BM_CALPHA=8'd2, BM_ADD=8'd4, BM_MUL=8'd5;

    // signed area x2 of triangle (a,b,c)
    function automatic longint edgef(input longint ax, ay, bx, by, cx, cy);
        edgef = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
    endfunction
    // round-to-nearest signed divide, den>0 (mirrors C divr)
    function automatic longint divr(input longint num, den);
        if (num >= 0) divr = (num + den/2)/den;
        else          divr = -(((-num) + den/2)/den);
    endfunction
    // top-left rule: 0 (inclusive on E==0) for a top/left edge a->b, else -1
    function automatic longint tlbias(input longint ax, ay, bx, by);
        tlbias = (((ay==by) && (bx<ax)) || (by>ay)) ? 64'sd0 : -64'sd1;
    endfunction
    // divide-free reductions (identical to the refmodel's div255/div31/div63_round)
    function automatic [5:0] red255(input [16:0] t); reg [17:0] m; begin m={1'b0,t}+18'd128; red255=(m+(m>>8))>>8; end endfunction
    function automatic [4:0] red31 (input [11:0] t); reg [12:0] m; begin m={1'b0,t}+13'd16;  red31 =(m+(m>>5))>>5; end endfunction
    function automatic [5:0] red63 (input [12:0] t); reg [13:0] m; begin m={1'b0,t}+14'd32;  red63 =(m+(m>>6))>>6; end endfunction
    // per-channel colour-mod: div255_round(ch*mod)
    function automatic [5:0] modch(input [5:0] ch, input [7:0] mod); modch = red255(ch*mod); endfunction

    longint x0,y0, x1n,y1n, x2n,y2n, area, den;
    longint u1a,v1a,u2a,v2a;
    longint r0a,g0a,b0a,a0a, r1a,g1a,b1a,a1a, r2a,g2a,b2a,a2a;
    longint sx,sy, b0,b1,b2, w0,w1,w2, uu,vv, ccr,ccg,ccb,cca;
    integer itu, itv, tw1, th1;

    reg [5:0] sr,sg,sb, tsr,tsg,tsb, dr,dg,db, o_or,o_og,o_ob;
    reg [6:0] ar,ag,ab;
    reg [7:0] ea,na;

    always @* begin
        // signed area of the raw winding; CCW-normalize by swapping verts b,c
        x0 = vx0; y0 = vy0;
        area = edgef(vx0,vy0, vx1,vy1, vx2,vy2);
        if (area < 0) begin
            x1n=vx2; y1n=vy2; x2n=vx1; y2n=vy1;
            u1a=vu2; v1a=vv2; u2a=vu1; v2a=vv1;
            r1a=vr2; g1a=vg2; b1a=vb2; a1a=va2;
            r2a=vr1; g2a=vg1; b2a=vb1; a2a=va1;
            area = -area;
        end else begin
            x1n=vx1; y1n=vy1; x2n=vx2; y2n=vy2;
            u1a=vu1; v1a=vv1; u2a=vu2; v2a=vv2;
            r1a=vr1; g1a=vg1; b1a=vb1; a1a=va1;
            r2a=vr2; g2a=vg2; b2a=vb2; a2a=va2;
        end
        r0a=vr0; g0a=vg0; b0a=vb0; a0a=va0;

        sx = ({48'd0,px} << 4) | 64'd8;   // pixel-center sample, 12.4
        sy = ({48'd0,py} << 4) | 64'd8;
        b0 = tlbias(x1n,y1n, x2n,y2n);    // edge b->c, opposite a
        b1 = tlbias(x2n,y2n, x0,y0);      // edge c->a, opposite b
        b2 = tlbias(x0,y0,   x1n,y1n);    // edge a->b, opposite c
        w0 = edgef(x1n,y1n, x2n,y2n, sx,sy) + b0;
        w1 = edgef(x2n,y2n, x0,y0,   sx,sy) + b1;
        w2 = edgef(x0,y0,   x1n,y1n, sx,sy) + b2;

        den = (area==0) ? 64'sd1 : area;   // guard /0; hit forced 0 below
        hit = (area != 0) && (w0>=0) && (w1>=0) && (w2>=0);

        uu  = divr(w0*{48'd0,vu0} + w1*u1a + w2*u2a, den);
        vv  = divr(w0*{48'd0,vv0} + w1*v1a + w2*v2a, den);
        ccr = divr(w0*r0a + w1*r1a + w2*r2a, den);
        ccg = divr(w0*g0a + w1*g1a + w2*g2a, den);
        ccb = divr(w0*b0a + w1*b1a + w2*b2a, den);
        cca = divr(w0*a0a + w1*a1a + w2*a2a, den);
        cr = ccr[7:0]; cg = ccg[7:0]; cb = ccb[7:0]; ca = cca[7:0];

        // nearest texel with clamp to [0,tex_w-1] x [0,tex_h-1]
        itu = (uu + 64'sd8) >>> 4;
        itv = (vv + 64'sd8) >>> 4;
        tw1 = tex_w - 1; th1 = tex_h - 1;
        if (itu < 0) itu = 0; else if (itu > tw1) itu = tw1;
        if (itv < 0) itv = 0; else if (itv > th1) itv = th1;
        tu = itu[15:0]; tv = itv[15:0];

        // ---- blend (texel/dst supplied by the FSM after its reads) ----
        sr = texel[15:11]; sg = texel[10:5]; sb = texel[4:0];
        tsr = modch({1'b0,sr}, cr);      // tinted source channels
        tsg = modch(sg, cg);
        tsb = modch({1'b0,sb}, cb);
        dr = dst[15:11]; dg = dst[10:5]; db = dst[4:0];
        // widen operands so the product isn't truncated to the 8-bit LHS context
        ea = ({8'd0,ca} * {8'd0,g_alpha}) / 16'd255;   // truncating, matches C (ca*alpha)/255
        na = 8'd255 - ea;
        o_or = tsr; o_og = tsg; o_ob = tsb;   // default: COPY/COLORKEY write tinted src
        case (blend_mode)
          BM_CALPHA: begin
            o_or = red255(tsr*ea + dr*na);
            o_og = red255(tsg*ea + dg*na);
            o_ob = red255(tsb*ea + db*na);
          end
          BM_ADD: begin
            ar = tsr + dr; ag = tsg + dg; ab = tsb + db;
            if (ar > 31) ar = 31; if (ag > 63) ag = 63; if (ab > 31) ab = 31;
            o_or = ar[5:0]; o_og = ag[5:0]; o_ob = ab[5:0];
          end
          BM_MUL: begin
            o_or = red31(tsr*dr); o_og = red63(tsg*dg); o_ob = red31(tsb*db);
          end
          default: ;
        endcase
        out_pix  = { o_or[4:0], o_og[5:0], o_ob[4:0] };
        write_en = !((blend_mode==BM_KEY) && (texel==colorkey));
    end
endmodule
`default_nettype wire

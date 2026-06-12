//  tb_blitter.sv — self-checking testbench: loads a scenario DDR image, runs
//  the blitter to frame completion, and diffs the target framebuffer against
//  the reference-model golden output (fb_expected.hex). Prints RESULT <name>.
`timescale 1ns/1ps
`default_nettype none
`include "blitter_defs.vh"

module tb_blitter;
    reg clk=0, rst=1;
    always #5 clk = ~clk;

    wire [31:0] addr; wire rd, wr; wire [63:0] din; wire [7:0] be;
    wire [63:0] dout; wire dout_ready, busy, idle;

    ddr_model u_mem(.clk(clk), .addr(addr), .rd(rd), .wr(wr), .din(din), .be(be),
                    .dout(dout), .dout_ready(dout_ready), .busy(busy));
    blitter_top u_blt(.clk(clk), .rst(rst), .mem_addr(addr), .mem_rd(rd), .mem_wr(wr),
                      .mem_din(din), .mem_be(be), .mem_dout(dout),
                      .mem_dout_ready(dout_ready), .mem_busy(busy), .idle(idle));

    reg [63:0] fbexp [0:`FB_QWORDS-1];
    integer i, errors, timeout, tbase;
    reg [8*32-1:0] name;

    initial begin
        if (!$value$plusargs("name=%s", name)) name = "scenario";
        $readmemh("ddr_init.hex", u_mem.mem);
        $readmemh("fb_expected.hex", fbexp);

        repeat (4) @(posedge clk);
        rst <= 0;

        timeout = 0;
        while ((u_mem.mem[`BLTCTRL_QW+`C_DONE][31:0] !==
                u_mem.mem[`BLTCTRL_QW+`C_SUBMIT][31:0]) && timeout < 8000000) begin
            @(posedge clk); timeout = timeout + 1;
        end
        repeat (4) @(posedge clk);

        tbase  = (u_mem.mem[`BLTCTRL_QW+`C_TARGET][0]) ? `FB1_QW : `FB0_QW;
        errors = 0;
        for (i = 0; i < `FB_QWORDS; i = i + 1)
            if (u_mem.mem[tbase+i] !== fbexp[i]) begin
                if (errors < 8)
                    $display("  MISMATCH qw %0d: got %h exp %h",
                             i, u_mem.mem[tbase+i], fbexp[i]);
                errors = errors + 1;
            end

        if (timeout >= 8000000) $display("RESULT %0s: FAIL (timeout)", name);
        else if (errors == 0)   $display("RESULT %0s: PASS", name);
        else                    $display("RESULT %0s: FAIL (%0d qword mismatches)", name, errors);
        $finish;
    end
endmodule
`default_nettype wire

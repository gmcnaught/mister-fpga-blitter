//  ddr_model.sv — behavioral qword memory with byte-enables for blitter sim.
//  Registered read (1-cycle), single-beat; never busy. Not a timing model —
//  it exists to verify blitter FUNCTION against the reference model.
`default_nettype none
`include "blitter_defs.vh"

module ddr_model #(parameter AW=32)(
    input  wire          clk,
    input  wire [AW-1:0] addr,
    input  wire          rd,
    input  wire          wr,
    input  wire [63:0]   din,
    input  wire [7:0]    be,
    output reg  [63:0]   dout,
    output reg           dout_ready,
    output wire          busy
);
    reg [63:0] mem [0:`MEM_QW-1];
    assign busy = 1'b0;
    integer b;
    always @(posedge clk) begin
        dout_ready <= rd;
        if (rd) dout <= mem[addr];
        if (wr) for (b=0; b<8; b=b+1)
            if (be[b]) mem[addr][b*8 +: 8] <= din[b*8 +: 8];
    end
endmodule
`default_nettype wire

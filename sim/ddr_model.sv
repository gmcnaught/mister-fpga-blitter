//  ddr_model.sv — behavioral qword memory with byte-enables + BACKPRESSURE.
//  Asserts `busy` 2 of every 3 cycles; only accepts an access (read/write) when
//  ~busy, and only returns dout_ready for an ACCEPTED read. This exercises the
//  blitter's mem_busy handling (a real shared f2h bus is rarely free). Not a
//  timing model — it verifies blitter FUNCTION against the reference model.
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

    // backpressure: free only 1 of every 3 cycles
    reg [1:0] bp = 2'd0;
    always @(posedge clk) bp <= (bp == 2'd2) ? 2'd0 : bp + 2'd1;
    assign busy = (bp != 2'd2);

    integer b;
    always @(posedge clk) begin
        dout_ready <= 1'b0;
        if (rd && !busy) begin           // accept read only when free
            dout <= mem[addr];
            dout_ready <= 1'b1;           // one beat, next cycle
        end
        if (wr && !busy) for (b=0; b<8; b=b+1)
            if (be[b]) mem[addr][b*8 +: 8] <= din[b*8 +: 8];
    end
endmodule
`default_nettype wire

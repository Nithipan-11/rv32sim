// Lockstep co-simulation testbench for the simple-riscv-cpu RTL.
//
// Loads a hex program into the CPU, then prints one Spike-style commit line per
// retired instruction, in exactly the format `rv32sim --trace` produces:
//
//   core   0: 3 0xPC (0xINSN) [xRD 0xVALUE] [mem 0xADDR 0xVALUE]
//
// Signals are sampled at each rising edge: the single-cycle datapath has settled
// on the current instruction, and the register/memory writes this edge performs
// are nonblocking, so the values read here are the ones about to commit. The run ends when an
// instruction jumps to itself (the programs end with `beq x0, x0, 0`).
`timescale 1ns/1ps

module tb_cosim;
    reg clk = 1'b0;
    reg reset = 1'b1;
    wire [31:0] pc;
    reg [1023:0] hexfile;
    integer max_cycles;
    integer cycle;

    cpu #(.IMEM_FILE("/dev/null")) uut (.clk(clk), .reset(reset), .pc_out(pc));

    always #5 clk = ~clk;

    initial begin
        if (!$value$plusargs("hex=%s", hexfile)) begin
            $display("usage: vvp tb_cosim +hex=program.hex [+max=N]");
            $finish;
        end
        if (!$value$plusargs("max=%d", max_cycles)) max_cycles = 10000;
        $readmemh(hexfile, uut.u_imem.mem);
        cycle = 0;
        #12 reset = 1'b0;
    end

    always @(posedge clk) begin
        if (!reset) begin
            if (uut.reg_write && uut.rd != 5'd0)
                $display("core   0: 3 0x%08x (0x%08x) x%-2d 0x%08x", pc, uut.instr, uut.rd, uut.write_back_data);
            else if (uut.mem_write)
                $display("core   0: 3 0x%08x (0x%08x) mem 0x%08x 0x%08x", pc, uut.instr, uut.alu_result, uut.reg2_data);
            else
                $display("core   0: 3 0x%08x (0x%08x)", pc, uut.instr);

            cycle = cycle + 1;
            if (uut.pc_next == pc) $finish;            // self-loop: program done
            if (cycle >= max_cycles) begin
                $display("TIMEOUT");
                $finish;
            end
        end
    end
endmodule

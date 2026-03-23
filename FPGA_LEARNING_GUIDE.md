# FPGA Learning Guide

A practical roadmap for getting into FPGA development, written for someone with
an electronics degree and experience building a 68K CPU emulator in software.

## Why FPGAs (and why now is a good time)

FPGAs let you design real digital circuits that execute in parallel hardware,
not sequentially on a CPU. The tooling has improved enormously since the late
'90s — open-source synthesis tools exist now, cheap dev boards are everywhere,
and the community has exploded thanks to retro computing and MiSTer.

Your 68k emulator project is directly relevant: people have implemented the
entire 68000 in Verilog on FPGAs (see the fx68k and TG68 cores). Your deep
understanding of the instruction set, bus timing, and flag behavior will
transfer directly.

## What's changed since your degree

Things you'll recognize:
- Flip-flops, multiplexers, state machines, tri-state buses — all still there
- Timing diagrams, setup/hold times — still matter
- Boolean algebra and Karnaugh maps — still the foundation

Things that are new:
- **HDLs replaced schematic capture** — you describe circuits in Verilog or VHDL,
  not by drawing gates (though you can still think in terms of gates)
- **Synthesis tools** compile HDL into actual FPGA bitstreams automatically
- **Simulation** is now software-based (no more oscilloscope-first debugging)
- **IP cores** — pre-built blocks (UARTs, DDR controllers, CPUs) you instantiate
  like library calls
- **Soft CPUs** — you can put a RISC-V or 68K core inside your FPGA design

## The two HDL languages

| | Verilog | VHDL |
|---|---|---|
| **Feel** | C-like syntax | Ada-like, verbose |
| **Community** | Dominant in US, retro/hobby | Dominant in Europe, aerospace |
| **Learning curve** | Lower | Higher (but more explicit) |
| **Recommendation** | **Start here** | Learn later if needed |

**Pick Verilog** (specifically SystemVerilog). It's more concise, has more
beginner tutorials, and most open-source retro computing cores (including 68K
implementations) are written in it.

## Recommended hardware

### Starter board: **Lattice iCE40** family (~$25–50)

- **iCEBreaker** or **TinyFPGA BX** — small, cheap, fully supported by
  open-source tools
- Why: You can go from HDL to bitstream using only free/open-source software
  (Yosys + nextpnr + icestorm). No vendor lock-in, no giant IDE installs
- Limitation: small FPGA, enough for LEDs/UART/SPI but not a full 68K core

### Next step: **Gowin GW1N/GW2A** (~$20–50) or **Lattice ECP5** (~$50–100)

- **Tang Nano 9K** (Gowin) — cheap, decent size, growing open-source support
- **ULX3S** or **OrangeCrab** (ECP5) — enough logic for a soft 68K core
- These have enough block RAM and logic cells for real projects

### If you get serious: **MiSTer FPGA** (DE10-Nano, ~$200+)

- Runs full system-on-chip recreations of Amiga, Genesis, arcade boards
- Uses Intel/Altera Cyclone V — needs vendor tools (Quartus, free tier)
- Huge community, many open-source cores to study
- Direct relevance to your Amiga emulation goal

### Boards to avoid for learning

- Xilinx/AMD boards (Basys 3, Arty) — fine hardware but the Vivado toolchain
  is 50+ GB and the learning curve is steeper. Save for later.

## Learning path

### Phase 1: Verilog fundamentals (1–2 weeks)

**Goal:** Understand how HDL maps to hardware. Blink an LED.

1. **Read "FPGA Design for Software Engineers"** — or work through the free
   Nandland tutorials (nandland.com/verilog). These are short and practical.

2. **Key concepts to internalize:**
   - Combinational vs sequential logic (wires vs registers)
   - `always @(posedge clk)` — this is the heartbeat of synchronous design
   - Blocking (`=`) vs non-blocking (`<=`) assignment — this trips up everyone
   - Modules and ports — like function signatures but for hardware blocks
   - `assign` for combinational logic, `always` for sequential

3. **First project: LED blinker**
   ```verilog
   module blink (
       input  wire clk,      // board crystal oscillator
       output reg  led = 0
   );
       reg [23:0] counter = 0;

       always @(posedge clk) begin
           counter <= counter + 1;
           if (counter == 0)
               led <= ~led;
       end
   endmodule
   ```
   This is the "Hello World" of FPGAs. Synthesize it, flash it, see the LED
   blink. You just built a hardware circuit.

4. **Second project: Button debouncer + LED toggle**
   - Teaches you about metastability (async inputs into sync domains)
   - Forces you to think about clock domains — no equivalent in software

5. **Simulation with Verilator or Icarus Verilog:**
   ```bash
   # Install (on Ubuntu/Debian)
   sudo apt install iverilog gtkwave

   # Compile and run a testbench
   iverilog -o sim blink_tb.v blink.v
   vvp sim
   gtkwave dump.vcd    # view waveforms
   ```
   Simulation is your printf-debugging equivalent. Get comfortable with it
   early — you cannot step through hardware with a debugger.

### Phase 2: Building blocks (2–4 weeks)

**Goal:** Build the components you'll need for a CPU. Each of these is a small
standalone project.

1. **UART transmitter** — shift register + baud rate generator. Send "Hello"
   to your PC over serial. This is satisfying and immediately useful for
   debugging everything that follows.

2. **UART receiver** — sampling, start/stop bit detection. Now you have
   bidirectional comms with your FPGA.

3. **Simple ALU** — add, subtract, AND, OR, XOR, shift. You already know
   exactly what this should do from your 68k emulator. Map your C `alu.c`
   logic into Verilog.

4. **Register file** — array of registers, dual-port read, single-port write.
   Think about how `cpu->d[0]`–`cpu->d[7]` would look as actual flip-flops.

5. **Block RAM / memory interface** — FPGAs have dedicated BRAM blocks. Learn
   to infer them from Verilog (hint: it's just an array with synchronous
   read/write).

6. **State machine** — implement a multi-cycle controller. This is how your
   68K fetch-decode-execute loop will work in hardware: a big FSM, not a
   `while` loop.

### Phase 3: A tiny CPU (4–8 weeks)

**Goal:** Implement a minimal CPU on your FPGA. Not a 68K yet — something
simpler to learn the patterns.

**Option A: Build your own 8-bit CPU from scratch**
- 8-bit data bus, 16-bit address space
- A handful of instructions: LOAD, STORE, ADD, SUB, JUMP, BRANCH
- Write a simple assembler in Python
- This teaches you the full pipeline: instruction fetch → decode → execute →
  writeback, all as FSM states

**Option B: Implement a subset of an existing ISA**
- RISC-V RV32I is popular and well-documented
- But honestly, a from-scratch toy CPU teaches you more about the tradeoffs

**What you'll learn:**
- How instruction decoding works in hardware (big `case` statements, not
  function pointer tables)
- How memory-mapped I/O works (directly — no OS abstraction)
- How bus arbitration and wait states work (directly relevant to 68K)
- The reality of multi-cycle operations in hardware

### Phase 4: 68K on FPGA (ongoing)

**Goal:** Port your 68K knowledge to hardware.

This is where your emulator experience becomes a superpower. You already know:
- Every instruction encoding and what flags it sets
- The effective address calculation logic
- The exception/interrupt model
- The bus protocol and timing

**Study existing cores first:**
- **fx68k** (Jorge Cwik) — cycle-accurate 68000 in SystemVerilog, used in
  MiSTer. Study this to see how a real implementation handles microcode.
- **TG68** — another popular 68K core, simpler but not cycle-accurate.
- **ao68000** — well-documented, open-source 68000 implementation.

**Then consider:**
- Implementing a subset yourself (start with MOVEQ, ADD, SUB, BRA — sound
  familiar?)
- Using your ProcessorTests suite to validate it (same tests, hardware target!)
- Eventually connecting it to VDP/sound chips for a Genesis or Amiga core

## Open-source toolchain (recommended for learning)

```
Your HDL code (.v files)
        │
        ▼
   ┌─────────┐
   │  Yosys   │  ← synthesis (HDL → netlist)
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │ nextpnr   │  ← place & route (netlist → FPGA layout)
   └────┬──────┘
        │
        ▼
   ┌───────────┐
   │ icestorm/  │  ← bitstream generation
   │ prjtrellis │
   └────┬──────┘
        │
        ▼
   FPGA board     ← flash via USB
```

Install everything on Linux:
```bash
# For iCE40 boards
sudo apt install yosys nextpnr-ice40 fpga-icestorm

# For ECP5 boards
sudo apt install yosys nextpnr-ecp5 prjtrellis
```

No vendor accounts, no 50 GB downloads, no license servers.

## Key mental shifts from software to hardware

| Software thinking | Hardware thinking |
|---|---|
| Code runs line by line | Everything executes simultaneously |
| Variables hold values | Wires carry signals, registers store state |
| Functions are called | Modules are instantiated (they always exist) |
| Loops iterate over time | Loops unroll into parallel hardware |
| `if/else` branches | `if/else` creates multiplexers |
| Memory is flat and fast | Memory is limited, explicitly managed |
| Debugging with printf | Debugging with waveforms (VCD viewer) |
| Optimization = algorithms | Optimization = timing closure + area |
| "It works" = done | "It meets timing at target frequency" = done |

## Recommended resources

### Books
- **"FPGA Prototyping by Verilog Examples"** (Pong Chu) — learn-by-doing,
  practical projects, excellent for self-study
- **"Digital Design and Computer Architecture"** (Harris & Harris) — modern
  textbook, covers Verilog and CPU design. Good refresher for your EE background
- **"Programming FPGAs"** (Simon Monk) — gentler intro if the above feel dense

### Free online
- **nandland.com** — short, clear Verilog tutorials with simulations
- **fpga4fun.com** — practical project-based tutorials
- **ZipCPU blog** (zipcpu.com) — deep dives on formal verification, bus
  protocols, and common mistakes. Written by an experienced FPGA engineer.
- **Nand2Tetris** (nand2tetris.org) — build a computer from gates up. You
  know most of this already, but it's a satisfying refresher in HDL form.

### Video
- **Ben Eater's breadboard CPU series** — not FPGA specifically, but excellent
  for visualizing how a CPU works at the gate level. You'll recognize
  everything from your 68K work.

### Community
- **1BitSquared Discord** — active FPGA hobbyist community
- **r/FPGA** on Reddit — mix of beginners and professionals
- **MiSTer FPGA forums** — directly relevant to retro computing on FPGAs

## How your 68k-emu project maps to FPGA

| Your C code | FPGA equivalent |
|---|---|
| `cpu->pc`, `cpu->d[]`, `cpu->a[]` | Register file (flip-flops) |
| `cpu_fetch()` | Bus master FSM: assert address, wait for data |
| `decode_and_execute()` | Big `case` on opcode bits (or microcode ROM) |
| `ea_resolve()` | Address calculation datapath + FSM |
| `cpu_set_flags()` | Combinational flag logic fed from ALU |
| `mem_read()` / `mem_write()` | Bus protocol state machine |
| `while (running)` loop | Clock-driven FSM (runs every cycle, forever) |
| Test ROMs in `tests.c` | Testbench stimulus in Verilog (or reuse via co-sim) |

## Suggested first week

1. Install Icarus Verilog and GTKWave (`sudo apt install iverilog gtkwave`)
2. Write the LED blinker module above
3. Write a testbench for it and view the waveform in GTKWave
4. Read the first 3 Nandland Verilog tutorials
5. If you have a board, synthesize and flash the blinker
6. If no board yet, order an iCEBreaker (~$35) or Tang Nano 9K (~$20)

You already have the hardest part down — understanding how a CPU actually works
at the instruction level. FPGA development is just expressing that knowledge in
a language the silicon understands.

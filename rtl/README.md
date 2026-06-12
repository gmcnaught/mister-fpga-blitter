# rtl/ — SystemVerilog blitter

The hardware blitter lands here, starting with the **#003 minimal RTL spike**:
one opaque rectangular copy (source DDR → framebuffer DDR), integrated beside the
video reader in a fork of the branded Solarus core, built via CI, proven on
hardware (DDR frame counter advances; mrext screenshot shows the copied rect).

Planned module split (mirrors the Cave/CV1000 core; see `../docs/`):

- `ring_reader.sv` — walks the DDR command ring until `END`, decodes 32-byte
  command words, drives the pipeline.
- `source_fetch.sv` — Avalon/f2h burst read master for source surfaces + format
  decode (RGB565 v1).
- `rect_blitter.sv` — per-command clip + flip + colorkey/const-alpha raster.
- `frame_buffer.sv` — on-chip line/tile buffer + burst-DMA write master to the
  DDR framebuffer; writes the video control word at end-of-frame.
- `blitter_top.sv` — top-level + the shared-f2h arbiter (scanout priority).

Each RTL block is verified against `../refmodel/` (the golden reference model):
identical command lists must produce bit-identical framebuffers in simulation
before any hardware build.

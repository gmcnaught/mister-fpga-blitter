# Lessons learned — building and hardware-validating the blitter

Everything below was learned (usually the hard way) taking this design from
the v1 single-transaction spike to the shipping architecture on real hardware,
driving the Solarus 1.6.5 engine on a DE10-Nano in the `solarus-mister`
integration repo. It is written for the next engine port (gmloader / OpenBOR)
and for anyone building a command-driven compositor on the Cyclone V HPS.

## How the architecture actually evolved

Each stage below shipped and was validated on hardware before the next
replaced it. The constant through all of it: the command protocol and the
bit-exact C reference model. Only the *transport* changed.

1. **v1 — single-transaction blitter into the DDR3 framebuffers** (this repo's
   `rtl/` spike). Functional, HW-proven, and immediately bandwidth-bound: every
   destination pixel was a read-modify-write across the shared f2h bus.
2. **Sources → SDRAM** (dedicated second bus). Then framebuffers and scanout
   too, behind a 3-client arbiter — because a line-buffered scanout reading
   DDR3 lost arbitration to composite traffic and starved.
3. **Per-pixel FSM → pipelined compositor** (`comp_pipeline`): band-chunked
   RMW, one pixel per clock, all blend modes native.
4. **Framebuffer → on-chip BRAM** with a vblank WORK→SCAN hardware snapshot.
   Destination preload/write-back — 44–66 % of compositor cycles — vanished
   (FILL ~1.05 cyc/px, COPY ~1.65 in sim). Scanout became same-cycle BRAM
   reads: the display deadline no longer touches any bus.
5. **Per-tile commands → tile lists** (`TILELIST`/`TILELIST_RES`): a map
   layer's thousands of draws collapse to one header command replayed by the
   fabric.
6. **Lazy staging → whole-quest SDRAM residency**: all atlases staged once at
   load (with an on-screen progress bar painted with plain FILL commands);
   sources never re-upload mid-game.
7. **Scan copy → DDR3 double-buffer** (Stage 5 Phase 2, 2026-07): `comp_fbram`
   was the largest M10K consumer at ~89 % BRAM utilization, so the SCAN half
   moved back off-chip (~160 M10K freed). WORK stays in BRAM — the RMW win of
   step 4 is untouched — but at frame-done the fabric burst-writes WORK to the
   inactive DDR3 framebuffer and flips banks; the reader fetches one 80-qword
   burst per scanline. The step-2 starvation that drove scanout *off* DDR3
   doesn't recur because composite traffic no longer touches DDR3 at all
   (sources in SDRAM, WORK in BRAM) — the bus carries only the two linear
   streams. Bonus: firing the snapshot immediately at frame-done instead of
   waiting for vblank was itself a major fps win.

The end state refines the original framing: the design started as "a blitter
that writes the framebuffer faster than the CPU" and ended as "a compositor
whose frame is *built* entirely on-chip" — external memory only ever sees the
finished image, once, as a linear burst.

## Transport and protocol

- **Keep per-pixel frame traffic off the shared HPS bus.** This is the single
  most important architectural rule. The f2h bus is shared with Linux, audio,
  and control traffic; scanout reading through it failed under composite load,
  and full-frame DMA from the CPU contended with everything. Every performance
  cliff in the project traced back to frame pixels crossing a shared bus.
  Stage 5 sharpened the rule's real shape: what the bus cannot absorb is
  *per-pixel, latency-coupled* traffic (RMW, per-qword scanout under
  contention) — a once-per-frame linear snapshot burst and line-granular
  scanout reads are fine, *provided nothing else on the bus is fighting them*.
- **Size the command ring for the pathological scene, not the average.**
  ~100 cmds/frame was the design estimate; dense 8×8-tile maps emitted >1250
  and overflowed the 1022-entry ring — the failure mode was a latched error
  and a black screen, two steps removed from the cause. The ring is 512 KiB
  now. Corollary: make overflow *loud* (an error latch the host can read beats
  a silent wrap by weeks of debugging).
- **Host and fabric constants are one ABI.** The ring capacity and the heap
  base moved together; deploying the engine and the bitstream separately
  produced phantom bugs on whichever side was stale. Treat
  `blitter_defs.vh` ↔ host header agreement as part of the contract, and
  deploy coupled changes atomically.
- **The control-block/ring boundary is part of the ABI.** Control qword 8
  aliases ring command 0; a control field appended there read cmd0's opcode
  bit and randomly enabled itself. New control bits went into spare bits of
  the last real control word instead.
- **Per-command flags beat global mode bits.** "Read sources from SDRAM" as a
  frame-global switch corrupted every transition frame that mixed staged and
  unstaged sources. The global bit demoted itself to a master enable; each
  command carries its own source-mux flag. Real frames are always mixed.
- **Staging needs an explicit coherency barrier.** A blit issued right after a
  `STAGE` must see the staged bytes: flush the staging write channel and
  invalidate the source-read cache between them. And a *mid-run* re-stage of a
  surface another path still references is how you corrupt resident memory —
  residency wants immutable, grow-only allocation (nothing ever frees the
  permanent region).
- **Walk-until-END + doorbell handshake never changed.** Submit/done sequence
  numbers with the doorbell store last survived four architecture revisions
  verbatim. Simple, orderable, debuggable from `devmem` — spend your novelty
  budget elsewhere.

## Cyclone V / SDRAM silicon

- **IOB-pack the SDRAM read capture.** A once-per-64-px column seam was
  months of red herrings and turned out to be the DQ input capture landing in
  fabric registers instead of the I/O block. Fix: a standalone reset-less
  `dout <= sdram_dq` register plus `FAST_INPUT_REGISTER` in the QSF — and the
  **wildcard form of that assignment is silently ignored**; it must name the
  exact path. Verify with the fitter report, not the source.
- **Never accept a multi-cycle ready signal by level.** The scanout reader
  accepted a 2-cycle `ok` twice and duplicated every even qword (an A,A,C,C
  banding pattern). Rising-edge-detect any handshake that can stay high.
  Guard it with a testbench that would have caught it (`tb_scan_qworddup`
  downstream) — this class of bug is trivially simulable once you know to
  look.
- **Know your SDRAM controller's actual concurrency.** The jtframe cache
  takes one request at a time (rising-edge `rd`, no queue, ~3 cycles per hit,
  serialized). A profiler model that assumed pipelined reads overstated the
  benefit of read-ahead; the real win was overlapping the *next span's* fetch
  with the *current span's* composite via a double-buffered line buffer.
- **Split multiplies across pipeline stages early.** The per-channel tint
  (multiply + /255 reduce) failed timing as one stage; splitting multiply from
  reduce (issue-interval unchanged, depth +1, bit-exact) closed it. On a
  ~100 MHz Cyclone V fabric, assume any `mul` feeding arithmetic needs its own
  stage.
- **Placement-seed-sensitive garbage is a real failure mode.** A marginal
  read-address mux worked or glitched depending on the fitter seed (one seed
  +0.107 ns = garbage, another +0.368 = clean). Pinning a good seed ships, but
  it's a band-aid — the fix is hardening the marginal path. Related: Quartus
  **builds an RBF even with negative slack**; gate deploys on the STA report,
  not on build success.
- **Cheap fault localization:** if the OSD (which bypasses the compositor) is
  clean but the game image is wrong, the fault is in the pixel datapath —
  memory, capture, scanout addressing — not clocks, PLLs, or the video DAC.

## Performance

- **The emit path becomes the bottleneck before the pixel path.** Once the
  fabric composited, the A9 spent ~112 ms/frame just *building and submitting*
  ~3 800 tile commands on heavy maps. Batching (tile lists) — not a faster
  emitter loop — was the answer: record a layer's entries once, replay with
  one command.
- **Record batch entries in map coordinates with a per-frame bias.** The first
  tile-list cut re-recorded entries on every camera move (a rebuild ~every
  frame while scrolling). Entries in map coords + a signed map→screen bias in
  the command header made the lists camera-independent: zero rebuilds whether
  standing or scrolling.
- **Destination RMW is the compositor's hidden cost.** Band preload + write-
  back to external memory dominated (44–66 % of cycles) even with a perfect
  pipeline. If the target fits in BRAM (320×240×16bpp = 150 KiB — it fits with
  room to spare on a Cyclone V SE), put it there and snapshot at vblank;
  "burst-DMA the composed bands out" (the CV1000 pattern this project started
  from) is strictly worse than not writing them out at all.
- **After offload, the frame budget belongs to the game.** With graphics at
  ~1–3 commands/layer, the remaining cost on heavy scenes was engine logic
  (entity movement bookkeeping, collision re-queries). Measure before assuming
  the next lever is also a graphics lever — a NEON/SIMD plan for the render
  path was refuted by one profiling session.
- **Sim cyc/px numbers are floors, not predictions.** They exclude bus
  arbitration, refresh, and cache misses. Put frame-cycle counters in the
  fabric and read them on hardware before claiming a throughput.

## Methodology (what actually made this tractable)

- **A bit-exact C golden model is the highest-leverage artifact in the repo.**
  Every RTL generation (FSM → pipeline → BRAM FB) shipped against the same
  software truth, diffed qword-for-qword in sim. Hardware debugging was
  thereby always about transport (buses, timing, caches) and never about pixel
  math. Divide-free reductions were proven exhaustively over their domain in
  the model first, then frozen.
- **Escape hatches hide bugs; eliminate them.** While unsupported draws could
  "escape" to software compositing, every fabric bug had a soft fallback that
  masked it (and dragged performance). Making every blend/tint native — then
  asserting `escape=0` in diagnostics — turned correctness regressions into
  visible, attributable failures.
- **Never A/B against a disconnected path.** The legacy software path still
  *ran* after scanout moved off DDR — it just produced black. Time was lost
  treating it as a reference. When an architecture change severs a debug
  path, delete it or mark it loudly; "runs but shows nothing" is worse than
  "removed."
- **Verify the deployed binary before blaming the RTL.** A stale library on
  the SD card (deploy tree not refreshed from the build tree) manufactured a
  phantom "hardware" bug more than once. Check a version marker (`strings |
  grep`) on-device before opening the RTL.
- **Quantify visual bugs.** "Seam looks better" doesn't survive contact with
  attract-mode fades and dithering. Filtering to static-content frames and
  measuring local-neighbor temporal variance turned a subjective seam into a
  2.2–4.1× vs 0.9–1.2× number that could gate a fix.
- **One change per hardware pass.** Every multi-change deploy that misbehaved
  cost more time attributing the regression than the serialized passes would
  have taken. (Exception that proved workable: coupled ABI constants, which
  *must* move together.)

## What transfers to the next engine port

The engine-agnostic pieces are exactly the ones in this repo: the protocol,
the reference model, and the host emitter (heap + SDRAM allocators included).
An engine binding has to provide three things:

1. a renderer shim that turns the engine's draw calls into emitter calls
   (Solarus: a subclass of its SDL renderer — ~one file),
2. an asset-load hook that uploads + `STAGE`s surfaces once (residency), and
3. a frame-pacing source (the scanout's `vsync_count` write).

The batching opcodes are tile-map-shaped but not Solarus-shaped: anything that
draws many rects from one texture with a shared blend (tile layers, particle
systems, bitmap fonts) fits `TILELIST`, and anything with a global animation
clock fits `TILELIST_RES`.

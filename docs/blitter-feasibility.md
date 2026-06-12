# Blitter feasibility & architecture study (go/no-go gate)

**Epic:** fpga-hw-blitter · **Task:** #001 (gate that protects the RTL tasks)
**Recommendation: GO** — conditional on the #003 spike confirming single-port DDR
arbitration + real fit/timing. Rationale below.

---

## 1. The frame to beat

Measured ([[solarus-perf-40fps]]): Solarus runs **~32 ms/frame (~31 fps)**,
dominated by **~19 ms** of A9 SDL software compositing (~80–150 blits of large
static-tile cells + sprites into a 320×240 RGB565 surface) + ~3 ms present. We
have **proven on HW** this is not fixable with cheaper pixels (RGB565, blend
tuning, colorkey-blit rewrite all ≈0). The A9 simply does too much per-pixel work;
the fabric is idle apart from scanout. **Target:** move the ~19 ms compositing off
the A9 so the frame supports **≥40 fps**.

## 2. DDR bandwidth budget (is bandwidth the new ceiling? — No)

DE10-Nano DDR3 via the f2h bridge sustains hundreds of MB/s to ~1 GB/s. Per-frame
demand at 60 Hz, worst case:

| Consumer                          | Bytes/frame | MB/s @60 |
|-----------------------------------|-------------|----------|
| Scanout read (existing reader)    | 153,600     | 9.2      |
| Blitter framebuffer write (1×)    | 153,600     | 9.2      |
| Blitter source reads (≈5× overdraw)| ~768,000   | ~46      |
| Audio ring                        | ~3,500      | 0.2      |
| **Total**                         |             | **~65**  |

~65 MB/s against a several-hundred-MB/s budget = **3×+ headroom**, even at a
pessimistic 5× overdraw and even before colorkey skip-writes reduce write traffic.
**Bandwidth is not the ceiling at 60 fps.** Key enabler (from the prior-art survey,
§4): keep DDR traffic as **long sequential bursts** by buffering on-chip — never
per-pixel DDR writes. The existing reader already bursts 80 beats/line; the blitter
writeback mirrors that.

## 3. Compute budget (does it fit the frame? — Yes, easily)

Worst case ~768K source pixels/frame (5× overdraw). A 64-bit datapath processes
**4 RGB565 px/clock**; at a 100 MHz fabric clock that's 768K/4/100M = **~1.9 ms**.
Even a scalar 1 px/clock path is ~7.7 ms — under the 16.7 ms frame and far under
the 19 ms A9 cost it replaces. Compositing is comfortably sub-frame.

## 4. Prior-art survey (summary; full doc: `../research-docs/research-mister-blitters.md`)

The **MiSTer Cave / CV1000 core** is the gold-standard analog: an SH-3 CPU emits
blit commands to a Cyclone FPGA that composites sprites into a DDR framebuffer —
structurally identical to A9 → Cyclone V → DDR. Decisive lessons adopted here:
- **Blit to an on-chip buffer with backpressure; burst-DMA to DDR.** (Cave
  `SpriteBlitter` → on-chip FB port → `SpriteFrameBuffer` DMA.) Retires the
  bandwidth risk's worst case.
- **Command word** ≈ CV1000 sprite config (pos/size/flip/blend/key); **walk the
  list until END** (Saturn VDP1). Colorkey **skip-write** = fast path.
- **Cull fully-clipped commands** (zero traffic). Page-flip double buffer on vBlank.
- CV1000 ran this on an **EP1C12** — far smaller than our 5CSEBA6 — strong
  evidence the pattern fits our fabric with room to spare.

## 5. Chosen architecture — drop-in producer + shared DDR port

The host build already writes a double-buffered framebuffer that `openbor_video_reader`
scans out via a control-word handshake. The blitter **replaces the ARM's software
compositing + buffer write** and reuses that exact handshake. **The reader is
unchanged.** The one integration constraint: `Solarus.sv` exposes a **single f2h
DDR master** (`DDRAM_*`, today muxed between cart-legacy and the video reader), so
the blitter shares it via a small arbiter (video scanout keeps priority; the
blitter composites the *inactive* buffer during the inter-frame window).

```
            0x3B00_0000 region                 0x3A00_0000 region
 ARM  ──▶  ┌──────────────┐   cmds   ┌────────────────┐   px   ┌──────────┐
 emit list │ command ring │────────▶ │  BLITTER core  │──────▶ │ BUF0/1   │
 + atlases │ source heap  │  source  │  RingReader→   │ burst  │ (fb)     │
           └──────────────┘────────▶ │  Fetch+decode→ │  DMA   └────┬─────┘
                ▲   doorbell          │  RectBlitter→  │              │ ctrl word
                │  (submit_seq)       │  ColorMix→     │──────────────┘ (frame_counter|buf)
                │                     │  on-chip FB ─DMA│              ▼
 poll done_seq ◀┘  ◀── done_seq ──────┤  ↕ shared f2h  │       openbor_video_reader
                                      └──────┬─────────┘         (UNCHANGED) ──▶ HDMI
                                             │ arbiter
                                      MiSTer DDRAM (f2h, single port)
```

Datapath stages (mirror the Cave module split): **RingReader** (FSM, walks the
ring until END) → **SourceFetch** (burst read master + format decode) →
**RectBlitter** (clip + flip + colorkey/const-alpha, per the reference model) →
on-chip **FrameBuffer** (line buffer + burst writeback) → writes the video control
word. Backpressure (`wait_n`/`ddr_busy`) on every memory access — mandatory
because DDR is shared with scanout + audio.

Protocol, command word, DDR map, and the bit-exact blend reduction are specified
in `blitter-protocol.md` and implemented + unit-tested in
`../refmodel/` (28/28 checks pass) — the golden model the RTL is diffed
against.

## 6. Fabric budget (estimate — confirm in #003)

Minimal rect blitter on the **5CSEBA6** (41,910 ALM / 553 M10K / 112 DSP):

| Block                              | ~ALM      | M10K | DSP |
|------------------------------------|-----------|------|-----|
| RingReader FSM + command decode    | 300–600   | 1    | 0   |
| Read/write masters + FIFOs         | 400–800   | 3–5  | 0   |
| RectBlitter (clip/flip/key)        | 300–600   | 0    | 0   |
| ColorMixer (const-alpha, 3 chan)   | 200–400   | 0    | 3–6 |
| On-chip line/tile buffer           | 100–200   | 2–4  | 0   |
| **Estimate**                       | **~1.5–3K** | **6–10** | **4–6** |

That's **~4–7 % of ALMs**. The Solarus core is light (no CPU emulation; the big
consumer is the sys `ascal` scaler), so headroom is expected to be ample. **The
real fit/timing baseline is established by #003** — flagged as the one number this
study estimates rather than measures (the branded-core fit report wasn't available
in-tree; build artifacts are gitignored).

## 7. Go/No-Go

**GO.** Every axis clears with margin:
- **Bandwidth:** ~65 MB/s vs several-hundred MB/s → 3×+ headroom.
- **Compute:** ~1.9 ms (4 px/clk) vs the 19 ms A9 cost it replaces → fits sub-frame.
- **Fabric:** ~4–7 % ALM estimate; CV1000 proved the pattern on a far smaller FPGA.
- **Integration:** clean drop-in producer reusing the proven double-buffer +
  control-word handshake; reader untouched.
- **Spec/model:** protocol fixed; golden reference model implemented + passing,
  so RTL (#003) and host emitter (#006) can proceed in parallel against it.

### Risks carried into downstream tasks (the spike-gate retires the top ones)
1. **Single f2h DDR port arbitration** with scanout under load → **#003** (the
   "can fabric Avalon-master the shared DDR + write a visible framebuffer" risk).
2. **Real fit + timing closure** on the branded core → **#003** baseline.
3. **Source upload/dirty-tracking bandwidth** at high overdraw / large atlases →
   **#005** (f2h vs ACP, upload-once static atlases).
4. **Per-pixel alpha** (if a quest needs it) → deferred; reserved in the format.

**Architecture is fixed for downstream tasks** (drop-in producer, shared f2h via
arbiter, command-ring + on-chip-buffer/burst-DMA, RGB565 v1, colorkey fast path).

# Migration plan — mister-fpga-blitter as a shared component

Three consumers now exist: **solarus-mister**, **gmloader-next**, and
**zaparoo-frontend** (with **maldita.castilla-mister** queued behind
gmloader's `tools/fabric_probe.c`). Each arrived at the blitter by a
different route and consumes it a different way. This document is the
strategy for rebasing all of them onto one shared component, and the
ordered migration that gets there.

Measurements below were taken against `mister-fpga-blitter@c4407e4`,
`solarus-mister@f079020`, `gmloader-next@d6d97eb`,
`zaparoo-frontend@e206d67`.

---

## 1. Where we actually are

Three consumers, three unrelated integration mechanisms:

| Repo | Mechanism | Consumes | State |
|---|---|---|---|
| `solarus-mister` | **copy-in vendoring** — 12 files hand-copied into `patches/mister/blitter/`, redistributed by `scripts/apply_mister_files.sh` | `blt_emitter`, `blt_alloc`, `grid_*`, `blitter_ref` | **forked**, bidirectionally |
| `gmloader-next` | **git submodule** `3rdparty/mfgpu` → this repo | `libmfgpu`, `host/`, `refmodel/` (include paths in `Makefile.gmloader:27-31`) | pin `9ccd57a` is **6 commits behind `master` and 4 commits off to the side** |
| `zaparoo-frontend` | **none** | — | no blitter dependency; hand-rolls its own DDR transport in `src/app/native_video_writer.cpp` |

### 1.1 The solarus fork is real and runs both ways

`diff` of this repo's canonical files against `solarus-mister/patches/mister/blitter/`:

| File | Lines only upstream | Lines only in solarus |
|---|---|---|
| `blt_wire.h` | 23 | 0 |
| `blitter_ref.h` | 92 | 13 |
| `blitter_ref.c` | 44 | 13 |
| `blt_emitter.h` | 36 | **61** |
| `blt_emitter.c` | 67 | **79** |
| `blt_alloc.{h,c}`, `grid_{cell,build,alloc,decompose,stats}.h` | — | — (identical) |

The two directions carry different features:

- **Upstream-only** — the MFGPU front-end: `BLT_OP_TRILIST = 12`,
  `BLT_OP_SET_TARGET = 13`, `BLT_TARGET_WORK` / `BLT_TARGET_APPSURF`,
  `BLT_F_SRC_SURFACE = 0x80` (the per-opcode shared-bit scheme), and the
  `blt_vtx_t` vertex path.
- **Solarus-only** — the command-ring double buffer: `dbuf_en`, `ring1`,
  `bank`, `sp_frame_cap`, the 256-entry deferred-free queue `dfq[]`, and
  the `blt_emitter_set_dbuf` / `blt_frame_ring` /
  `blt_emitter_free_deferred` API.

Neither side is a superset. **There is currently no single revision of the
emitter that both shipping consumers could adopt.** That is the first thing
the migration has to fix, and nothing else can proceed in front of it.

### 1.2 The fabric lives inside a consumer

`mister-fpga-blitter/rtl/` holds only the v1 spike. The production fabric —
**6,911 lines** of SystemVerilog across 26 files plus **44 testbenches** —
lives in `solarus-mister/fpga/rtl/` and `fpga/sim/`. So does the
host↔fabric contract gate, `scripts/tests/test_wire_constants.py`, which
cross-checks `blitter_ref.h` against `blitter_defs.vh` and hardcodes
solarus paths on both sides.

That means gmloader (and maldita, and any future zaparoo work) depends on a
bitstream produced by a *different application's* repo, and the only
automated defence against host↔fabric constant drift is a script that only
runs in that one repo, against that one repo's vendored copy — the copy
we just established has drifted.

### 1.3 A whole layer is triplicated that this repo doesn't even own

The MiSTer **transport** — getting frames and audio across the f2h bus —
exists in all three consumers and in none of them shared:

| | solarus | gmloader | zaparoo |
|---|---|---|---|
| `native_video_writer` | 90 lines C | 137 lines C | 336 lines C++ |
| geometry | 320×240 RGB565 | 288×216 RGB565 | 352×240 / 720×480 / … RGB8888 |
| layout @ `0x3A000000` | ctrl `+0x0`, bufs `+0x40` / `+0x40040` | same offsets | word0 `+0x0`, word1 `+0x4`, bufs `+0x1000` / `+0x180000` |
| `native_audio_writer` | 139 lines | 150 lines (51 lines drifted) | — |
| `mister_native_audio.cpp` | ✔ | ✔ (688 lines drifted) | — |
| `fps_overlay.h` | ✔ | ✔ (marked "Solarus port") | — |

solarus and gmloader agree on the DDR layout and disagree on geometry;
zaparoo uses a different contract entirely ("Menu fork v2", a different
reader RTL). This is the layer zaparoo actually needs — it needs the
transport, not the compositor — and it is the layer least represented in
the shared repo today.

### 1.4 Licensing is a hard gate, not a footnote

| Repo | License | Can link GPL-3.0? |
|---|---|---|
| `mister-fpga-blitter` | GPL-3.0 | — |
| `solarus-mister` | GPL-3.0 (Solarus engine + OpenBOR core lineage) | ✅ |
| `gmloader-next` | GPL-2.0-**or-later** | ✅ (combined work becomes GPL-3.0; `fabric_probe.c` already declares this) |
| `zaparoo-frontend` | **PolyForm Noncommercial 1.0.0** | ❌ **no** |

PolyForm Noncommercial restricts commercial use; GPL-3.0 forbids adding
that restriction. zaparoo-frontend **cannot statically link this repo as
it is licensed today**, and no amount of build-system work changes that.

The relevant fact: `git shortlog -sne --all` on this repo shows **a single
author across all 30 commits**. The host-side C (`host/`, `refmodel/`,
`libmfgpu/`) is wholly ours, clean-room by construction, with no
third-party code to encumber it. It is relicensable by decision.

The RTL is not. `solarus-mister/fpga/rtl/jtframe/` is vendored from
jotego's jtcores (see its `PROVENANCE.md`) and `openbor_video_reader.sv`
descends from the MiSTer OpenBOR core. **Tier 5 stays GPL-3.0
permanently.** That is fine — a bitstream is a separate work; shipping a
GPL `.rbf` next to a PolyForm userspace binary raises no combination
question.

---

## 2. Target architecture — tier the component by who can consume it

The license split and the dependency split land on the same seams, which
is what makes this tractable. Five tiers, each with its own consumers and
its own license:

```
 Tier 0  wire contract      blitter_ref.h, blt_wire.h, blitter_defs.vh, blitter-protocol.md
         └─ consumers: everyone, including the fabric.       ~data only

 Tier 1  reference model    blitter_ref.c, blt_tri.c, the golden-vector suites
         └─ consumers: test builds only, never shipped.

 Tier 2  host emitter       blt_emitter, blt_alloc, grid_{cell,build,alloc,decompose,stats}
         └─ consumers: solarus, gmloader, maldita.

 Tier 3  geometry front-end libmfgpu (transform / cull / batch assembly)
         └─ consumers: gmloader, maldita.

 Tier 4  MiSTer transport   native video + audio writers, DDR mmap helper, fps overlay
         └─ consumers: ALL FOUR, including zaparoo.          ← the zaparoo unlock

 Tier 5  fabric             comp_pipeline, comp_fbram, fb_ddr_writer, ddr3_scan_adapter,
                            sdram_fb_cache, jtframe, + 44 testbenches       GPL-3.0, fixed
         └─ consumers: whoever synthesises a core.
```

**Proposed licensing:** Tiers 0–4 → **MPL-2.0**. Tier 5 → stays GPL-3.0.

MPL-2.0 is the right pick over LGPL or BSD: it is file-level copyleft, so
improvements to the shared files flow back (which is the entire point of
making it shared), while imposing no license on the consuming work. It
satisfies GPL-2.0-or-later (gmloader), GPL-3.0 (solarus), and PolyForm
(zaparoo) simultaneously. BSD/MIT would also work but gives up the
give-back; LGPL's relinking clause is awkward for a statically-linked ARM32
binary and buys nothing MPL doesn't.

**Target repo layout:**

```
mister-fpga-blitter/
  docs/            protocol spec (Tier 0 normative), lessons, this plan
  refmodel/        Tier 1
  host/            Tier 2
  libmfgpu/        Tier 3
  platform/mister/ Tier 4   ← NEW: video/audio writers, mmap, fps overlay
  rtl/  sim/       Tier 5   ← the production fabric, moved here from solarus
  tests/wire/      test_wire_constants.py, promoted and de-solarused
```

Every consumer takes the whole repo as one submodule and compiles only the
tiers it needs. One pin, one contract, one place drift can be detected.

---

## 3. Migration

Six phases. The ordering is load-bearing: each phase's gate is the next
phase's precondition, and Phase 1 in particular cannot be deferred — every
later phase assumes one canonical emitter exists.

### Phase 1 — Reconcile the fork *(blocking; nothing else starts first)*

Land solarus's ring-double-buffer work into this repo's `host/` alongside
the existing MFGPU work, producing one emitter that is a true superset.

- Port `dbuf_en` / `ring1` / `bank` / `sp_frame_cap`, the `dfq[]`
  deferred-free queue, and the three new API entry points into
  `blt_emitter.{h,c}`.
- Reconcile `blitter_ref.{h,c}` and `blt_wire.h` — mostly upstream-ahead,
  so this is applying solarus's 13/13/0 lines onto our tree, not a merge.
- Re-run the existing gates: `refmodel`, `host`, `libmfgpu`, `sim`
  (34 + 28 + transform + 17 scenarios).
- Confirm the `dbuf_en == 0` path is byte-identical to today, as
  `blt_emitter.h`'s own comment promises. That is what makes this
  behaviour-neutral for gmloader.
- Tag **`contract-v1.0`**.

*Gate:* all four suites green, and a `dbuf`-off emitter run produces
bit-identical command rings to `contract-v1.0~1`.

*Risk:* low-moderate. Mechanical, but it touches the file both shipping
consumers run on. The `dbuf`-off identity check is the safety net.

### Phase 2 — Promote the contract gate

Move `test_wire_constants.py` from solarus into `tests/wire/`, parameterised
on paths rather than hardcoding solarus's tree, and wire it into CI here.

Until Phase 4 moves the RTL, point it at a submodule/checkout of
solarus-mister's `fpga/rtl/`. It is worth running cross-repo in the interim
precisely because that is where the drift risk lives.

*Gate:* the gate runs in this repo's CI and passes against
`solarus-mister@HEAD`.

### Phase 3 — One consumption mechanism: submodule + generated copy

Standardise on **git submodule**, because gmloader already proves it and it
gives an auditable pin.

- **gmloader**: bump `3rdparty/mfgpu` from `9ccd57a` to `contract-v1.0`.
  Its side-branch commits should be inspected and either landed in Phase 1
  or discarded; a pin that is 4 commits off `master` is a second latent
  fork.
- **solarus**: add the same submodule. `apply_mister_files.sh:31` keeps
  copying files into the engine tree — the patch-series build needs that —
  but it copies **from the submodule**, and `patches/mister/blitter/` stops
  being hand-editable. Add a `make check-vendor` that diffs the copy
  against the submodule and fails on mismatch.

That last gate is the actual fix. The mechanism was never the problem;
*undetected* divergence was. A generated copy with a CI diff is as safe as
direct linkage and doesn't disturb the patch-series build.

*Gate:* both repos build and pass their own suites on `contract-v1.0`;
`check-vendor` is red if you hand-edit a vendored file.

*Risk:* moderate for solarus — first time its emitter is not
locally-editable. Expect one or two "we patched this locally" discoveries;
each is a Phase-1 backport.

### Phase 4 — Move the fabric here

Relocate `fpga/rtl/` and `fpga/sim/` from solarus-mister into this repo's
`rtl/` and `sim/`, retiring the v1 spike or keeping it under `rtl/spike/`.
solarus-mister keeps `Solarus.qsf` / `.sdc` / `sys/` and consumes the RTL
from the submodule for synthesis.

Why this is worth the disruption: it makes Phase 2's gate an *intra-repo*
test, gives gmloader and maldita a fabric that isn't downstream of another
game, and puts the 44 testbenches next to the reference model they are
diffed against. Keep `jtframe/PROVENANCE.md` intact through the move.

*Gate:* a bitstream built from the submodule is bit-identical to one built
from solarus's tree at the same revision; all 44 testbenches pass here.

*Risk:* **highest phase.** Quartus path/`files.qip` plumbing is fiddly and
a broken synthesis path is invisible until a build. Do this as its own
change, verify with an identical-bitstream check, and keep solarus's tree
in place until that check passes.

### Phase 5 — Extract Tier 4 (the transport)

Create `platform/mister/` with one video writer and one audio writer:

- A **mode table** parameterising geometry and pixel format, seeded with
  the union of what exists: 320×240 RGB565 (solarus), 288×216 RGB565
  (gmloader), 352×240 / 720×480 RGB8888 (zaparoo).
- **Both DDR layouts** behind a layout descriptor — the `+0x40` / `+0x40040`
  RGB565 form and the Menu-fork-v2 `+0x1000` / `+0x180000` form. Do not
  try to unify them in software; they are consumed by two different reader
  RTLs. Unify the *readers* separately, if ever, and that is a Tier 5 job.
- Shared `mmap` / `O_SYNC` open helper (all three hand-roll the identical
  pattern) and `fps_overlay.h`.
- Fold in the 51 drifted lines of `native_audio_writer` and the 688 of
  `mister_native_audio.cpp` — these need a real reconciliation, not a
  pick-one.

Migrate solarus and gmloader onto it first; they are already GPL and
already share a layout, so they are the low-risk proof.

*Gate:* solarus and gmloader both render and play audio on hardware from
the shared writers with no visual or audio regression.

### Phase 6 — Onboard zaparoo-frontend

Gated on the **relicense decision** (§4). With Tiers 0–4 under MPL-2.0:

- zaparoo adds the submodule, links `platform/mister/`, and deletes
  `src/app/native_video_writer.cpp` (336 lines) in favour of the shared
  writer configured for the Menu-fork-v2 layout. Its
  `zaparoo_rust_crt_{h,v}_offset()` hooks stay — pass them in as writer
  config rather than `extern "C"` reach-ins.
- CMake integration is straightforward: Tier 4 is plain C with no
  dependencies.

**Explicitly out of scope:** driving the blitter from QML. Turning Qt's
software renderer into a display list is a far larger project than this
migration and should not be bundled into it. zaparoo's win here is the
transport layer and a pinned contract; a `SPRITELIST`/`TILEMAP`-driven list
view is a separate proposal on top of a working Tier 4.

*If the relicense is declined:* zaparoo keeps its own transport
implementation but consumes **Tier 0 only** as a generated header (contract
constants are facts, not creative expression, and can ship under a
permissive notice), plus the `check-vendor`-style CI diff. That preserves
contract sync without a link. The other option — a separate GPL helper
process talking to zaparoo over shared memory — is clean legally but adds a
process boundary to a frame-critical path, and is not recommended.

---

## 4. Decisions needed before Phase 1

1. **Relicense Tiers 0–4 to MPL-2.0?** Sole authorship makes it a
   decision, not a negotiation. Everything about zaparoo's onboarding turns
   on it, and Phase 6 shape changes completely by the answer. Worth
   settling now even though it isn't needed until Phase 6, because it
   determines whether Tier 4 gets designed as shared-by-four or
   shared-by-three.
2. **Does the fabric move (Phase 4)?** The alternative — leave RTL in
   solarus, extract only `blitter_defs.vh` and the testbenches — is cheaper
   and lower risk, but leaves gmloader/maldita/zaparoo permanently
   downstream of a game repo for their bitstream. Recommend the move; note
   that Phases 1–3 and 5 stand on their own if it is deferred.
3. **gmloader's 4 side-branch commits.** These need reading before the pin
   is bumped. If they carry real work, they are Phase-1 inputs.
4. **Repo naming.** If Phase 4 lands, "blitter" undersells what the repo
   holds (contract + model + emitter + geometry front-end + transport +
   fabric). Renaming is cheap now and expensive after four consumers pin it.

## 5. Ordering summary

```
  P1 reconcile fork ──┬── P2 promote wire gate ──┐
   (blocking)         │                          ├── P4 move fabric  (optional, high risk)
                      └── P3 one mechanism ──────┤
                                                 └── P5 extract Tier 4 ── P6 zaparoo
                                                                            ↑
                                                              relicense decision (§4.1)
```

Phases 2 and 3 are independent of each other and can run in parallel once
Phase 1 tags `contract-v1.0`. Phase 5 needs Phase 3's mechanism in place;
Phase 6 needs Phase 5 and the license answer. Phase 4 can slot in any time
after Phase 2 or be dropped without invalidating the rest.

The single highest-value change is **Phase 1** — until one emitter revision
exists that both shipping consumers can run, "shared component" is
aspirational.

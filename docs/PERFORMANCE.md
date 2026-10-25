# Performance: where the time goes and what is left

Measured on a 4-vCPU cloud VM (about 2.8 GHz), Linux, headless, `AE2_NODRAW=1` (the game,
VU, VIF and GS front end run, nothing is rasterised), so it shows the CPU cost that every
renderer pays. Frames were checked against earlier builds with `AE2_DUMP_EVERY` after each change.

## Where things stand

- **One player:** the game runs at its full 50 fps with drawing off, title to stage. With the
  GPU renderer the remaining cost is the GPU's; the software renderer is the slow option
  (about half the CPU time of a software-rendered run is `span_sse_t2z1`, textured depth-tested spans).
- **Two players, split screen:** each game frame runs twice (once per camera), so this is the
  case worth optimising. The 2-player benchmark below takes about 32 s instead of 50 s at
  the start of this work (drawing off); with VU1 on its own thread (MTVU) the game logic, the
  geometry work and the GS front end now run on three cores at the same time.

## Done

| change | effect |
|---|---|
| VU1 programs for characters (0x60) and stage geometry (0x2138, backface 0x2000) written by hand in C (`vu1_native.c`) | port 20 → 22: about 17% less CPU in 2-player |
| Split-screen RAM undo in 4-page blocks (`AE2_P2RAMBLOCK`) | fewer page faults per frame |
| VIF unpack: one specialised loop per data format and mask/offset variant | `do_unpack` 6.7% → 4.1% of CPU |
| VU0 macro ops store results with one SIMD blend instead of lane by lane | about 10% less time in `v_madda4`, `v_mul4` and the rest |
| VU0 MAC flags (vi17) only computed with `-DAE2_VU0_MAC`: this game never reads them | a few % |
| Texture uploads (`trx_row`) write in block-column runs, one address computation per run | 6.6% → 4.3% of CPU |
| VU clamp drops its denormal test: guest threads run with flush-to-zero/denormals-are-zero (now also set on ARM, where sse2neon's `_mm_setcsr` only set the rounding mode) | about 5% overall; the clamp follows every VU multiply/add |
| **MTVU:** VIF1 and the VU1 microprograms run on their own thread, between the EE and the GS thread: the EE queues its VIF1 data and GS input to it, and it passes the VU1 output and the EE's GS input on to the GS thread in the original order. The game, VU1 and the GS front end run on three cores | 2-player benchmark 48.6 s → about 31-32 s, 1-player 41.9 s → about 27 s (drawing off). Frames identical with it on and off |
| `-O2` instead of `-O1` for the generated code | no measurable gain (not adopted) |

The first rows together: about 12% less CPU in 2-player split screen, frames bit-identical to
before. MTVU then spreads the remaining work over three cores.

MTVU details: the EE thread still walks the DMA chains (p2.c's split-screen hooks see them)
and copies the VIF1 data into the VU1 thread's queue; that thread runs in the guest FPU mode
(round toward zero, flush to zero), the GS thread in the host's. An EE access to VU1 memory
waits for both queues to drain. `AE2_MTSTAT=1` prints every 250 frames how busy the VU1 and
GS threads were and how long the EE waited for them. `AE2_MTVU=0` turns it off. Like the GS
thread it is off with `AE2_DETERMINISTIC=1`; `AE2_MTGS=2` forces both on for testing.

## Measured and set aside

- **A draw-only second pass.** Timing each step of the game's frame (`0x19cf28`) in player 2's
  pass: the game logic (actor lists run by `0x192ff8`, camera, input) takes under 1 ms; about
  90% of the pass is `0x193238`'s kick of the draw list, i.e. VIF/VU1/GIF work for player 2's
  view, which is needed anyway. Skipping the logic would save little.
- **Overlapping the two views on two cores.** Both views run their geometry through the same
  VU1 state in turn, so only player 1's logic (about 1 ms) could overlap with player 2's drawing.
- **Hand-writing more of the stage program.** The remaining compiled time (`vp1_2000_dde3`,
  `vp1_2508`) is the batches the native kernel hands back because they need the clipper at
  0x2420, whose results depend on VU flag timing.

## Profile (2 players, stage, drawing off; before the second round of changes)

| share | what | idea |
|---|---|---|
| ~11% | `vn1_0060` hand-written character program | about half was the clamp (now cheaper); a 4-vertex-at-a-time (SoA) version |
| ~10% | libc (`memmove`, `mprotect`) | split-screen page saves; VIF copies |
| ~7% | `vp1_2000_dde3` compiled stage program | the clipper batches (see above) |
| ~4% | `trx_row` texture uploads | SIMD within a run |
| ~5% | `stage_kernel` / ~3% `stage_backface_kernel` | already hand-written; SIMD across vertices |
| ~4% | GIF packet parsing (`gif_transfer_impl`, `vertex_kick`) | skip re-parsing state that did not change |
| ~8% | VU0 macro ops (`v_madda4` etc.) | inlining the ops into the generated code |
| ~3% | `vp1_2508` stage clipper (compiled) | flag-timing-heavy; hard to hand-write |
| ~12% | the game's own code (`f_*`) | already native C; little to gain without changing the game |

## Bigger ideas

- **More than one VU1 thread** (measured, set aside): in a 2-player stage there are about
  2,000 VU1 program runs per frame of a few microseconds each, and between almost every two
  of them a DIRECT GS packet or a program without a hand-written version that must run in
  order (on average about 4 hand-written runs in a row). Handing such small pieces to more
  threads costs about as much as the work, and with VU1 on its own core the EE thread
  (game logic, split-screen page saves) is close to being the limit anyway.
- **Profile-guided optimisation** (`-fprofile-use`) of the generated code: a few percent,
  but every builder would need to run a profiling pass.

## How to measure

```sh
AE2_NODRAW=1 AE2_DETERMINISTIC=1 AE2_TURBO=1 AE2_EXIT_AT=2900 AE2_NOAUDIO=1 \
AE2_P2=1 AE2_P2SPLIT=1 AE2_PROF=1 AE2_PROF_RAW=raw.txt \
AE2_INPUT="m1+350:start;m1+356:-start;1450:cross;1456:-cross;1700:cross;1706:-cross;1900:cross;1906:-cross;2600:stick=0,-127" \
port/ae2 --headless game
```

`AE2_PROF` (x86-64 Linux) prints the hottest functions; `AE2_PROF_RAW` writes raw offsets
to resolve with `nm -n port/ae2` (static functions are missing from the built-in report).
Run each measurement twice: the noise on a cloud VM is about ±5%.

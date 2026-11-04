# MVEX-OpticalFlow

One monitor at half its refresh, doubled in real time by a Xeon Phi 7120A: AMD's FSR 3 optical flow, ported to the card's MVEX vectors, on two teams of 120 threads over 60 cores.

KWin casts the monitor at half the refresh. The card makes the frame half way between each two. A click-through overlay on the same monitor shows generated, real, generated, real. On a 1366x768 monitor at 59.79 Hz that is 29.895 frames a second in and 59.79 pictures a second out. It doubles whatever is on the monitor: a video, a window being dragged, a scrolling page. The pointer stays the compositor's own, on top, at the full refresh. The monitor's mode and scale are not touched.

## The chain

```
Frame-gen > FSR > deblocking
```

| Stage | What it is | Whose | Team | ms, median |
|---|---|---|---|---|
| Frame-gen | Motion field at the picture's own size, 171 x 96 blocks of 8 x 8. Every pixel picks its own vector among those around it, by where the two frames agree over 5 x 5. At an odd motion, the mean of the two aligned pairs either side of the true middle | Search, filter, upscale: AMD's, through `framegen.c`. The rest: here | first, t0 t1 | 10.3 |
| FSR | FSR 3.1.6's temporal pass at the picture's own size: no upscale, no sharpening (RCAS off), on luma, the change carried to the colours | AMD's, through the port in `fsr3.c`, with a floor on the new frame's weight | first | 15.1 |
| Deblocking | H.265's normal luma filter, 8.7.2.5.7, on each colour, line by line, across the 8 x 8 grid, only where two blocks' vectors differ by a pixel | The standard's. **AMD ships no deblocking to port**: FidelityFX has none, AMF only an on/off for its hardware encoder | second, t2 t3 | 0.47 |

With frame in and picture out, 1.2 ms each, plus landing and pyramid: **28.6 ms of 33.45**. The chain is for frame generation's output. A real frame has been through no frame generation and has no seams; it goes to the glass as it came.

[docs/INDEX.md](docs/INDEX.md) says what is AMD's and what is not. [docs/measurements.md](docs/measurements.md) has the numbers, [docs/limitations.md](docs/limitations.md) what is not solved.

```
╔═ desktop ═══════════════════════════════════════════════════════════════════════════════════╗
║  ┌───────────────┐  BGRx, no pointer   ┌──────────────────┐  one copy, x byte set to 255    ║
║  │ KWin cast     │ ──────────────────▶ │ mvexflow         │ ───────────────┐                ║
║  │ half refresh  │  PipeWire, memfd    │ identical frame: │                ▼                ║
║  └───────▲───────┘                     │ dropped          │   ┌──────────────────────────┐  ║
║          │ excludeFromCapture          └──────────────────┘   │ /dev/shm/phi-hostmem     │  ║
║  ┌───────┴───────────────────────────────────────────────┐    │ [1920, 1984) MiB         │  ║
║  │ overlay: wlr layer surface, wp_fifo, wp_presentation  │ ◀──┤ 6 real, 6 generated      │  ║
║  │ generated, real, generated, real, click-through       │    └───┬──────────────────▲───┘  ║
║  └───────────────────────────────────────────────────────┘        │                  │      ║
╚═ PCIe, the card's own DMA channel ════════════════════════════════╪══════════════════╪══════╝
╔═ Xeon Phi 7120A: mvexflowd ═══════════════════════════════════════╪══════════════════╪══════╗
║  ┌────────────────────────────────────────────────────────────────▼──────────────────┴────┐ ║
║  │ first team, 120 threads, t0 t1: luma, pyramid, flow, mid, pick, FSR                    │ ║
║  │ second team, 120 threads, t2 t3: deblocking                                            │ ║
║  └────────────────────────────────────────────────────────────────────────────────────────┘ ║
║  core 60: cpu 0, block pollers moved to cpus 241 and 242 for the session (tools/pollmove)   ║
╚═════════════════════════════════════════════════════════════════════════════════════════════╝
```

## Use

```
scripts/mvexflow.sh start     # card program, overlay; the monitor is doubled
scripts/mvexflow.sh status
scripts/mvexflow.sh stop      # the monitor is plain again; pages and pollers back as they were
```

The stages can be chosen, each measured in `docs/measurements.md`:

```
scripts/mvexflow.sh start --stages 2   # frame-gen and deblocking, no FSR: 13.6 ms a frame, closer to the truth, a refresh or two less latency
scripts/mvexflow.sh start --stages 0   # frame-gen alone
scripts/mvexflow.sh start --half       # the port's own form: field at half size, windows over 16 x 16 tiles
scripts/mvexflow.sh start --phase 0    # whole-pixel samples: the sharpest (.964), half a pixel from the true middle at odd motion (29.281 dB)
scripts/mvexflow.sh start --fine 4 --stages 2   # 342 x 192 vectors, no FSR: 22.6 ms
```

`local.conf` (not tracked) says which monitor and which program opens the cast:

```
MVEXFLOW_OUTPUT=NAME-OF-THE-MONITOR
DESK=/path/to/desk
```

## What it needs

- A Xeon Phi x100 booted with the stack of `Intel-Phi` projects: the `phi` command, the shared region, the KNC clang (`config.mk`).
- KDE Plasma on Wayland: KWin 6.8 was used. `zwlr_layer_shell_v1`, `wp_fifo_manager_v1`, `wp_presentation`, `wp_viewporter`, `excludeFromCapture`.
- A program allowed to open KWin's `zkde_screencast_unstable_v1` for an output, which runs a child with the PipeWire node in place of `@NODE@`. Here that is a private tool, `desk cast --output NAME --no-cursor -- CMD @NODE@`. It is not in this repository.
- libpipewire and libwayland-client headers, wayland-scanner.

## Gates

| Program | Runs on | Proves |
|---|---|---|
| `mf_replay --ref` | card, host lane model | every generated pixel equal on both, a control proves it can fail |
| `mf_replay` | card | dB, sharpness and seams against a real frame left out; ms per stage |
| `mf_replay --model N` | host | the same picture, no card |
| `scripts/live_test.sh` | card, the monitor | rates, cadence and latency on the real output, with test motion under the overlay |
| `scripts/pace_test.sh` | card, the monitors | missed refreshes with and without it, here and on another monitor |
| `tools/probe.c` | the monitor | the overlay lands 1:1, and is hidden from the cast |
| `scripts/compdb.sh` | host | clangd over every source reports nothing |

## Licences

The search, filter and upscale derive through `framegen.c`, and `fsr3.c` derives, from the FidelityFX SDK, Copyright (C) Advanced Micro Devices, Inc., MIT: `LICENSES/FidelityFX-MIT.txt`. `tools/pollmove/pollmove.c` is GPL-2.0, being a kernel module. `tools/protocols/` holds the Wayland protocols under their own notices.

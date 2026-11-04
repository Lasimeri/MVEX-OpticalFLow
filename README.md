# MVEX-OpticalFlow

One monitor at half its refresh, doubled in real time by a Xeon Phi 7120A: AMD's FSR 3 optical flow, ported to the card's MVEX vectors, on 120 threads of 60 cores.

KWin casts the monitor at half the refresh. The card makes the frame half way between each two. A click-through overlay on the same monitor shows generated, real, generated, real. On a 1366x768 monitor at 59.79 Hz that is 29.895 frames a second in and 59.79 pictures a second out. It doubles whatever is on the monitor: a video, a window being dragged, a scrolling page. The pointer stays the compositor's own, on top, at the full refresh. The monitor's mode and scale are not touched.

The port comes from [Intel-Phi-VisionTrack](https://github.com/Lasimeri/Intel-Phi-VisionTrack) (`src/kern/framegen.c`, unchanged but for one threshold made a setting). [docs/INDEX.md](docs/INDEX.md) says what is AMD's and what is not. New here: colour, since the port was grey; the host program; the card program; and the picture, pixel for pixel.

## Pixel for pixel

The port built its motion field at half the picture's size, to fit a tracker's budget. A motion of an odd number of pixels could not be represented there, so the two frames met a pixel apart before they were averaged: a double edge. Here the field is at the picture's own size, 171 x 96 blocks of 8 x 8, and every pixel picks its own vector among those around it, the one on which the two frames agree over a 5 x 5 window. Samples are whole pixels of both frames, `a` at `x + (v >> 1)` and `b` at `x - (v - (v >> 1))`, the same place of the scene, so their mean is as sharp as they are and nothing is resampled.

| Against a real frame left out, moving tiles | Field at half size | Pixel for pixel | Mean of the two frames |
|---|---|---|---|
| Sharpness, 1.000 = the truth's | 0.911 | **0.978** | 0.867 |
| dB | **33.860** | 29.152 | 28.556 |

The dB fall is the trade, not a fault: at an odd motion a sharp picture sits half a pixel off the true middle, and only resampling reaches that place, which is the blur this exists to remove. `mvexflow --half` still runs the port's way. Which looks better is the viewer's to say.

```
╔═ desktop ═══════════════════════════════════════════════════════════════════════════════════╗
║                                                                                             ║
║  ┌───────────────┐  BGRx, no pointer   ┌──────────────────┐  one copy, x byte set to 255    ║
║  │ KWin cast     │ ──────────────────▶ │ mvexflow         │ ───────────────┐                ║
║  │ half refresh  │  PipeWire, memfd    │ identical frame: │                ▼                ║
║  └───────────────┘                     │ dropped          │   ┌──────────────────────────┐  ║
║          ▲                             └──────────────────┘   │ /dev/shm/phi-hostmem     │  ║
║          │ excludeFromCapture (excl.sh)                       │ [1920, 1984) MiB         │  ║
║          │                                                    │ 6 real, 6 generated      │  ║
║  ┌───────┴───────────────────────────────────────────────┐    └───┬──────────────────▲───┘  ║
║  │ overlay: wlr layer surface, wp_fifo, wp_presentation  │ ◀──────┤ no copy          │      ║
║  │ generated, real, generated, real, click-through       │  wl_shm pool on that file │      ║
║  └───────────────────────────────────────────────────────┘        │                  │      ║
╚═ PCIe, the card's own DMA ════════════════════════════════════════╪══════════════════╪══════╝
╔═ Xeon Phi 7120A ══════════════════════════════════════════════════╪══════════════════╪══════╗
║  ┌────────────────────────────────────────────────────────────────▼──────────────────┴────┐ ║
║  │ mvexflowd: frame in, luma, pyramid, search, filter, upscale, mid, pick, frame out      │ ║
║  │ 120 threads: t0, t1 of cores 0 to 59. Block pollers moved to cpus 241, 242, core 60.   │ ║
║  └────────────────────────────────────────────────────────────────────────────────────────┘ ║
╚═════════════════════════════════════════════════════════════════════════════════════════════╝
```

## Use

```
scripts/mvexflow.sh start     # card program, overlay; the monitor is doubled
scripts/mvexflow.sh status
scripts/mvexflow.sh stop      # the monitor is plain again; pages and pollers back as they were
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
| `mf_replay` | card | dB against a real frame left out, against the mean of the two and against the older frame again; ms per stage |
| `scripts/live_test.sh` | card, the monitor | rates, cadence and latency on the real output, with test motion under the overlay |
| `scripts/pace_test.sh` | card, the monitors | missed refreshes with and without it, here and on another monitor |
| `tools/probe.c` | the monitor | the overlay lands 1:1, and is hidden from the cast |
| `scripts/compdb.sh` | host | clangd over every source reports nothing |

Numbers: [docs/measurements.md](docs/measurements.md). What is not solved: [docs/limitations.md](docs/limitations.md).

## Licences

The search, filter and upscale derive through `framegen.c` from the FidelityFX SDK, Copyright (C) Advanced Micro Devices, Inc., MIT: `LICENSES/FidelityFX-MIT.txt`. `tools/pollmove/pollmove.c` is GPL-2.0, being a kernel module. `tools/protocols/` holds the Wayland protocols under their own notices.

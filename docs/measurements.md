# Measurements

All on one Xeon Phi 7120A (61 cores, TSC 1238 MHz, Gen2 x8) and one desktop running KWin 6.8.2, 2026-11-04. Budget per source frame: 33.45 ms.

Real footage below is a true 59.94 fps clip scaled to 1366x768, every second frame given, the frame between held out as truth: exactly a 30 to 60 doubling against what really happened. It is not in the repository. Odd scenes move an odd number of pixels a given frame, the frame between lying on a half pixel, rendered by a 2x box.

**dB**: tiles that moved, RGB, against the truth. **Sharp**: green differences right and below against the truth's, 1.000 = as sharp as what really happened. **Seams**: the step across the 8 pixel grid over the step inside blocks, against the truth's, 1.000 = no more seams than what really happened.

## The chain, on the card

Real footage, 20 frames, 60 x 2 first team, 60 x 2 second team, 240 threads on cpus 1 to 240, read on the card. Sharpness .869 against .867 for the mean of the two frames, whose motions here are mostly slow. Median us: DMA in 1225, land 186, pyramid 281, histograms 222, search 3072, filter 1056, upscale 639, mid 1159, pick 4115, FSR 15189, deblock 489, DMA out 1215. **Total 28844, max 31321.** Card = lane model: 0 of 19,932,672 pixels differ, control caught 19 of 19; 0 of 5,245,440 on each of window, scroll, pan7, still. Audit: 0 illegal, 0 suspect, 2346 KNC vector.

Three rounds, interleaved, total ms: whole chain 28.725, 28.635, 28.625 (max 29.197, 31.956, 29.070); frame-gen and deblocking without FSR 13.679, 13.633, 13.616 (max 14.1).

FSR's stages, us a picture: clear 533, refine 335, finish 61, mark 357, input 121, hpass 641, diff 1012, mips 326, sc 206, react 1135, inst 2909, acc 6506, merge 135.

| Frames, on the card | dB | Mean of two | Older again | Seams |
|---|---|---|---|---|
| Real footage | 33.072 | 28.556 | 25.462 | 1.002 |
| Window, 12 and 4 px a frame | 26.310 | 10.859 | 9.393 | 1.013 |
| Scroll, 20 px a frame | 32.971 | 10.186 | 8.695 | 1.002 |
| Pan, 7 px a frame | 24.178 | 11.794 | 10.406 | 0.998 |

## What each stage earns, real footage, host lane model, 12 frames

| Chain | dB | Sharp | Seams | Real frame after it, against itself |
|---|---|---|---|---|
| Frame-gen alone | 32.678 | .876 | 1.014 | identical |
| + deblock, tc 2, beta 12 (the default) | 32.665 | .873 | 1.001 | identical |
| + deblock, tc 4, beta 24 | 32.636 | .870 | .988 | identical |
| + deblock, tc 16, beta 96 | 32.233 | .859 | .943 | identical |
| + FSR at AMD's weights, generated and real frames | 26.353 | .789 | 1.028 | 28.293 dB |
| + FSR, new frame at least 75 %, generated and real frames | 32.937 | .868 | 1.002 | 44.510 dB |
| + FSR, generated only, at least 75 % | 32.075 | .842 | 1.004 | identical |
| + FSR, generated only, at least 90 % | 32.622 | .864 | 1.001 | identical |
| **+ FSR, generated only, at least 95 % (the default)** | **32.706** | **.871** | **1.000** | identical |
| + FSR, generated only, 100 %: no FSR at all | 32.737 | .876 | 1.000 | identical |

FSR through both kinds of frame, at 75 %, is the one form in which it earns anything on real footage: +.272 dB. It costs 46.8 ms a frame, past the budget. On generated pictures alone it never helps real footage; on scenes it earns +.020 dB (scroll) and +.046 dB (window) at 95 %. At AMD's own weights the damage is misregistered history under one vector a 16 x 16 block: a still scene through it reads 99 dB, and a floor on the reactive value, up to 100 %, changed nothing (28.504 dB).

## Forms of the picture, host lane model, dB, no FSR, no deblocking

| Frames | Half-size field | Whole-pixel | Phase-correct (the default) |
|---|---|---|---|
| Real footage | **32.906** | 28.875 | 32.210 |
| Pan, 7 px | 24.117 | 18.941 | **24.176** |
| Vertical, 7 px | 25.312 | 19.938 | **25.513** |
| Both, 5 and 3 px | 21.384 | 16.645 | **21.805** |
| Scroll, 20 px | 29.527 | 32.883 | **32.896** |
| Window | 25.810 | 26.238 | **26.245** |
| Pan, 12 px | 39.782 | **57.713** | **57.713** |
| Fast box, 90 px | **10.210** | 9.387 | 9.472 (mean of two 10.399) |

Sharpness on real footage: half-size .909, whole-pixel .977, phase-correct .883. Whole-pixel is the sharpest and the furthest from the truth: at an odd motion it is half a pixel from the true middle. The half-size form stays closer to the truth on real footage, and loses everywhere a whole-pixel motion can be exact.

The picture's edge: 100.0 % of the error on the pan scene and 88.9 % on scroll lay within 32 px of it, content arriving in one frame only. The frame that has the place, alone, took pan from 37.3 to 56.25 dB and scroll from 28.2 to 32.94.

## Vector grid

Real footage, on the card, whole chain: cells of 8 pixels (171 x 96 vectors, the default) 33.284 dB, 46.8 ms; cells of 4 pixels (342 x 192) 33.251 dB, 57.7 ms. On the host, no FSR, cells of 8 / 4 / 2: pan 56.252 / 55.603 / 55.993; scroll 32.940 / 32.965 / 32.893; window 26.257 / 25.898 / 25.814; real 28.657 / 28.479 / 28.480 whole-pixel.

Tried and thrown out on these numbers: vectors projected forward to the middle instant (+.009 to +.065 dB without); credit in the pick for the texture a vector lines up (worse in every scene, sharpness to 2.383).

## Fused against plain, half-size field

Total ms, 59 x 2, 3 rounds: fused 6.49 to 6.51, plain 8.19 to 8.21; picture .94 against 2.33. Fused: 16 BGRA pixels a register, byte-wise means in 32-bit lanes. Plain: three Q6 F32 planes through the port's `fg_blend`. Fused gives up .007 dB.

## On the monitor

Test motion under the overlay (`tools/moving.c`), the whole chain, 13 s: cast 29.92/s, generated 29.92/s, glass 59.54/s, 771 of 774 pictures one refresh after the last, 3 two refreshes, 2 discards, 0 late, 0 skipped; card mean 29.04, max 34.70 ms; cast stamp to scanout 85 to 90 ms.

The whole chain, real use, 129 s: identical 0.11/s, generated 29.77/s, glass 59.36/s, 25 discards, 1 skipped, 0 late; card mean 24.24, max 34.50 ms; 55 to 60 ms for 57 s, 70 to 75 ms for 68 s.

Frame-gen and deblocking without FSR, real use, 216 s: glass 58.78/s, 6 discards, card mean 23.84 ms (a build with a needless copy since removed; 13.6 ms now), 45 to 65 ms.

Frame-gen alone, half-size field: 37.9 to 47.8 ms.

The team's stand-down: a real session of 993 s, 27,823 frames, one team back 2 times at 25.6 ms; both teams 66 ms. A frame after a pause reaches the glass without waiting for them.

## Design probes (`tools/probe.c`)

| Question | Result |
|---|---|
| Overlay 1:1 at KWin scale 0.75, 1821 x 1024 logical | cast equals buffer in 94,417,920 of 94,417,920 pixels |
| `excludeFromCapture` on a layer surface | 6 of 93,368,832 pixels still match (chance) |
| Cast with `maxFramerate` = refresh / 2 | BGRx 1366x768, 29.895 to 29.903 /s; x byte always 255 |

## Threads and pollers

60 x 2 against 59 x 2, half-size field, 4 rounds: 6.428 to 6.468 ms against 6.450 to 6.484. Pollers moved against left at boot, 3 rounds: 6.476 6.472 6.487 against 6.490 6.463 6.464: no measurable gain while they are idle. The second team waiting at 8 cycles a look slowed the first by about 15 %; at 1024 search is back at 3.07 ms from 3.54.

## Cost to the rest of the desktop

`scripts/pace_test.sh`, half-size field, 20 s, 2 rounds interleaved, test motion in all passes, an invisible probe on each monitor:

| | Main monitor, 120 Hz, missed | This monitor, missed | `kwin_wayland`, % of a core |
|---|---|---|---|
| Without, round 1 | 0 of 2,400 | 0 of 1,195 | 14.5 |
| With, round 1 | 0 of 2,399 | 0 of 1,195 | 21.4 |
| Without, round 2 | 105 of 2,294, cause unknown | 3 of 1,192 | 17.5 |
| With, round 2 | 0 of 2,398 | 1 of 1,194 | 23.0 |

## The viewer

"Seems like it's working! Least for the test pattern." The chain since: to be judged by the same eye.

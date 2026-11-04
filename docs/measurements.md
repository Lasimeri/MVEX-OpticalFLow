# Measurements

All on one Xeon Phi 7120A (61 cores, TSC 1238 MHz, Gen2 x8) and one desktop running KWin 6.8.2, 2026-11-04. Budget per generated frame: 33.45 ms.

Real footage below is a true 59.94 fps clip scaled to 1366x768, every second frame given, the frame between held out as truth: exactly a 30 to 60 doubling against what really happened. It is not in the repository.

## Pixel for pixel, the default

Field at the picture's size, a vector per pixel, whole-pixel samples, 60 x 2, real footage, 20 frames. Median us: DMA in 1226, land 178, pyramid 257, histograms 211, search 4212, filter 966, upscale 603, mid 1219, picture 2307, DMA out 1215, **total 12,430, max 13,005**, of 33,450. Card = lane model: 0 of 19,932,672 pixels differ, control caught 19 of 19; 0 of 5,245,440 on each of window, scroll, pan7, both53, still. Audit: 0 illegal, 0 suspect, 778 KNC vector.

Early-out on the finest level, rounds interleaved: threshold 0: 12.508, 12.527 ms; 1024 (the default): 12.425, 12.429 ms; 8192 (the port's): 10.924, 10.962 ms, with 29.218 dB and sharpness 0.977 against 29.152 and 0.978. The prediction from the level above is always even, so a loose threshold keeps faint detail on an even vector.

dB on tiles that moved, and sharpness on them against the truth (differences in green, right and below; 1.000 = the truth's), half-size field then pixel for pixel. Odd scenes move an odd number of pixels a given frame, the frame between being rendered on the half pixel. Synthetic: 7 frames, host lane model; real footage: 19 frames, card.

| Frames | dB, half | dB, pixel | dB, mean of two | Sharp, half | Sharp, pixel | Sharp, mean |
|---|---|---|---|---|---|---|
| Real footage | 33.860 | 29.152 | 28.556 | .911 | **.978** | .867 |
| Pan, 7 px a frame | 24.117 | 18.916 | 11.791 | .897 | **.999** | .911 |
| Vertical, 7 px a frame | 25.312 | 19.858 | 11.952 | .937 | **.997** | .907 |
| Both, 5 and 3 px a frame | 21.384 | 16.638 | 11.934 | .857 | **.998** | .905 |
| Pan, 12 px a frame | 39.782 | 37.366 | 10.880 | .999 | .999 | .966 |
| Scroll, 20 px a frame | 29.527 | 28.223 | 10.188 | .994 | .997 | .961 |
| Window, 12 and 4 px a frame | 25.810 | **26.238** | 10.875 | 1.006 | 1.011 | .967 |
| Fast box, 90 px a frame | 10.210 | **9.418** | **10.399** | 1.003 | 1.109 | .970 |

Tried and thrown out on these numbers: vectors projected forward to the middle instant (at best +0 dB, -.009 to -.065 against none, on window, scroll, fast and real footage); credit in the pick for the texture a vector lines up (worse in every scene at every strength, sharpness up to 2.383: it invents edges).

On the monitor, pixel for pixel, real use: glass 59 to 60 pictures/s when the content moves at the full rate, half generated; card 10.2 to 11.5 ms; cast stamp to scanout 40.7 to 58.6 ms, mostly 57.5, against 37.9 to 47.8 at the half-size field: which refresh a picture catches depends on where the cast falls against the refresh, and at 12 ms a frame it now often misses one. In 64 s of use: cast 29.89/s, glass 59.61/s, 2 discards, card mean 11.36 ms, max 26.19 (the team coming back).

## The team's stand-down

One real session of 993 s, half-size field, on the viewer's own desktop: 27,823 frames, the team brought back 2 times, mean 25.6 ms, max 26.3 ms. A frame after a pause reaches the glass without waiting for it. In the same session: cast 29.89/s, 1.89/s of it identical, 27.97 generated/s, glass 55.92/s, 49 discards (one in 20 s), 1 generated frame skipped to take back the drift.

## Design probes (`tools/probe.c`)

| Question | Result |
|---|---|
| Overlay 1:1 at KWin scale 0.75, 1821 x 1024 logical | cast equals buffer in 94,417,920 of 94,417,920 pixels |
| `excludeFromCapture` on a layer surface | 6 of 93,368,832 pixels still match (chance) |
| Cast with `maxFramerate` = refresh / 2 | BGRx 1366x768, 29.895 to 29.903 /s; x byte always 255 |

## Card against host lane model

`mf_replay --ref`: 0 of 7,343,616 pixels differ on each of window, pan, scroll, fast, still (7 generated frames each), 0 of 19,932,672 on real footage (19 frames), control caught every frame. Audit: 0 illegal, 0 suspect, 705 KNC vector.

## Picture, half-size field

dB on tiles that moved, RGB, against the truth:

| Frames | Generated | Mean of two | Older again | Moved blocks |
|---|---|---|---|---|
| Real footage | 33.860 | 28.556 | 25.462 | 64.2 % |
| Synthetic pan | 39.782 | 10.880 | 9.490 | 99.8 % |
| Synthetic scroll | 29.527 | 10.188 | 8.695 | 58.1 % |
| Synthetic window | 25.810 | 10.875 | 9.406 | 16.3 % |
| Synthetic fast box, 90 px a frame | **10.210** | **10.399** | 8.500 | 2.0 %: **generated loses to the mean** |

These synthetic scenes move even distances only, which is why this table never showed the half-size field's flaw.

## Time, half-size field

Half-size field, real footage, median us, 60 cores x 2: DMA in 1226, land 179, pyramid 245, histograms 212, search 1449, filter 426, upscale 267, mid 283, picture 931, DMA out 1215, total 6464, max 6800.

| Threads | Fused total ms | Plain total ms | Fused picture | Plain picture |
|---|---|---|---|---|
| 59 x 2 | 6.49 to 6.51 | 8.19 to 8.21 | .94 | 2.33 |
| 59 x 3 | 6.53 to 6.56 | 7.86 to 7.94 | .78 to .79 | 1.82 to 1.83 |
| 59 x 4 | 6.94 to 6.97 | 8.16 | .70 | 1.63 |

3 rounds, interleaved. Fused: 16 BGRA pixels a register, byte-wise means in 32-bit lanes. Plain: three Q6 F32 planes through the port's `fg_blend`. Fused gives up .007 dB on moving tiles (33.860 against 33.867).

| Threads, pollers | Total ms, rounds interleaved |
|---|---|
| 60 x 2, pollers on 238, 239 | 6.428 6.449 6.442 6.468 |
| 59 x 2, pollers on 238, 239 | 6.482 6.484 6.450 6.468 |
| 60 x 2, pollers moved to 241, 242 | 6.476 6.472 6.487 |
| 60 x 2, pollers on 238, 239 (same run) | 6.490 6.463 6.464 |

Placement read on the card during a session: 120 tasks, `Cpus_allowed` 1 2 5 6 ... 233 234 237 238. Moving the pollers gives no measurable gain while they are idle.

## On the monitor

Test motion under the overlay (`tools/moving.c`), 12 s, 59 x 2: glass 59 to 60 pictures/s, half generated; 58 to 60 of 60 pictures one refresh after the last; 3 discards; card 5.9 ms; cast stamp to scanout 41.2 to 41.5 ms; `kwin_wayland` 17.4 %, `mvexflow` 2.0 % of a core.

Real footage in mpv under the overlay, 15 s: glass 59 to 60 pictures/s, 0 discards during playback, card 6.2 to 6.45 ms, 47.4 to 47.8 ms. One second fell to 10 pictures/s with a 953 ms gap while the cast kept delivering identical frames: the player stalled.

A still desktop: 1 frame/s or less reached the card and the glass.

## Cost to the rest of the desktop

`scripts/pace_test.sh`, 20 s, 2 rounds interleaved, test motion in all passes, an invisible probe on each monitor:

| | Main monitor, 120 Hz, missed | This monitor, missed | `kwin_wayland`, % of a core |
|---|---|---|---|
| Without, round 1 | 0 of 2,400 | 0 of 1,195 | 14.5 |
| With, round 1 | 0 of 2,399 | 0 of 1,195 | 21.4 |
| Without, round 2 | 105 of 2,294, cause unknown | 3 of 1,192 | 17.5 |
| With, round 2 | 0 of 2,398 | 1 of 1,194 | 23.0 |

## The viewer

"Seems like it's working! Least for the test pattern." Then the order that made the picture pixel for pixel. Whether it now looks right: to be judged by the same eye.

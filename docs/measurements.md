# Measurements

All on one Xeon Phi 7120A (61 cores, TSC 1238 MHz, Gen2 x8) and one desktop running KWin 6.8.2, 2026-11-04. Budget per generated frame: 33.45 ms.

Real footage below is a true 59.94 fps clip scaled to 1366x768, every second frame given, the frame between held out as truth: exactly a 30 to 60 doubling against what really happened. It is not in the repository.

## Design probes (`tools/probe.c`)

| Question | Result |
|---|---|
| Overlay 1:1 at KWin scale 0.75, 1821 x 1024 logical | cast equals buffer in 94,417,920 of 94,417,920 pixels |
| `excludeFromCapture` on a layer surface | 6 of 93,368,832 pixels still match (chance) |
| Cast with `maxFramerate` = refresh / 2 | BGRx 1366x768, 29.895 to 29.903 /s; x byte always 255 |

## Card against host lane model

`mf_replay --ref`: 0 of 7,343,616 pixels differ on each of window, pan, scroll, fast, still (7 generated frames each), 0 of 19,932,672 on real footage (19 frames), control caught every frame. Audit: 0 illegal, 0 suspect, 705 KNC vector.

## Picture

dB on tiles that moved, RGB, against the truth:

| Frames | Generated | Mean of two | Older again | Moved blocks |
|---|---|---|---|---|
| Real footage | 33.860 | 28.556 | 25.462 | 64.2 % |
| Synthetic pan | 39.782 | 10.880 | 9.490 | 99.8 % |
| Synthetic scroll | 29.527 | 10.188 | 8.695 | 58.1 % |
| Synthetic window | 25.810 | 10.875 | 9.406 | 16.3 % |
| Synthetic fast box, 90 px a frame | **10.210** | **10.399** | 8.500 | 2.0 %: **generated loses to the mean** |

The synthetic scenes move even distances only.

## Time

Real footage, median us, 60 cores x 2: DMA in 1226, land 179, pyramid 245, histograms 212, search 1449, filter 426, upscale 267, mid 283, picture 931, DMA out 1215, total 6464, max 6800.

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

"Seems like it's working! Least for the test pattern." Real content: to be judged by the same eye.

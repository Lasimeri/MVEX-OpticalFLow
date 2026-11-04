# Limitations

Stated as measured. See [measurements.md](measurements.md).

- **"60" is the monitor's 59.79 Hz, "30" is 29.895.** The monitor has no 60.00 mode at this size.
- **A sharp generated picture can sit half a pixel off.** At an odd motion the samples are whole pixels of both frames, aligned with each other, half a pixel from the true middle. Against truth that costs dB: 29.152 against 33.860 on real footage. Only resampling reaches the true place, and that is the blur this removes. Slow motion of 1 px a frame therefore advances in steps of a whole pixel, every second picture repeating the place of the last.
- **Small fast things double.** A 120 px box at 90 px a source frame: 9.418 dB pixel for pixel, 10.210 at the half-size field, 10.399 for the plain mean. Not overcome: a fine search of 8 x 8 blocks does not catch it, and projecting vectors forward did not help.
- **Uncovered areas ghost.** Where a thing was hidden in one of the two frames, the mean of the two has nothing to agree on.
- **Latency.** 40.7 to 58.6 ms from the cast's stamp to scanout pixel for pixel, in two groups a refresh apart, 40 to 45 ms and 55 to 60 ms, against 37.9 to 47.8 at the half-size field, on top of what the source itself takes: at 12.4 ms a frame the picture often misses the next refresh. Measured from KWin's stamp and presentation feedback, never with a camera on the glass.
- **Refreshes are missed now and then.** 0 to 3 in 20 s on this output, with or without the doubler; 49 in 993 s of real use. The compositor's, cause not found.
- **Drift.** The cast stamp to scanout rises about .03 ms/s; generated frames are dropped to take it back: 1 in 993 s of real use.
- **The port's picture was never compared against AMD's on a GPU.** Its deviations are in [INDEX.md](INDEX.md).
- **The block pollers' move buys nothing measurable while they are idle.** Under block traffic it was not measured. It takes a one-shot kernel module (`tools/pollmove`), marks the kernel tainted (flag 4096), and a card reboot undoes it. `scripts/card.sh stop` restores them, because Intel-Phi-VisionTrack finds its spare core by their old place.
- **What the stand-down saves is not known in watts.** The card's board sensors read nothing here and the host has no UPS tool. Die temperatures went from 48-53 C to 52-57 C with the team spinning.
- **The region part overlaps vgd's claim** ([1792, 2048) MiB, that project being stopped). `scripts/card.sh` refuses to start beside another card program; nothing stops another from starting after it.
- **`card.sh` refuses by an allow-list of what the card may be running.** `FORCE=1` overrides it.

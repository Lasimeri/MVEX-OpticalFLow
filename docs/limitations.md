# Limitations

Stated as measured. See [measurements.md](measurements.md).

- **"60" is the monitor's 59.79 Hz, "30" is 29.895.** The monitor has no 60.00 mode at this size.
- **Motion is at half resolution.** The field is built at half the picture's size, as the port's budget on the tracker demanded, so a motion of an odd number of pixels cannot be represented, and the two frames meet one pixel apart before they are averaged. Being replaced by a field at full resolution.
- **Small fast things double.** A 120 px box at 90 px a source frame: 10.210 dB against 10.399 for the plain mean.
- **Uncovered areas ghost.** Where a thing was hidden in one of the two frames, the mean of the two has nothing to agree on.
- **Latency.** 37.9 to 47.8 ms from the cast's stamp to scanout, on top of what the source itself takes. Measured from KWin's stamp and presentation feedback, never with a camera on the glass.
- **Refreshes are missed now and then.** 0 to 3 in 20 s on this output, with or without the doubler: the compositor's, not found.
- **Drift.** The cast stamp to scanout rises about .03 ms/s. By arithmetic one generated frame is dropped every 9 minutes or so to take it back. Not observed.
- **The port's picture was never compared against AMD's on a GPU.** Its deviations are in [INDEX.md](INDEX.md).
- **The block pollers' move buys nothing measurable while they are idle.** Under block traffic it was not measured. It takes a one-shot kernel module (`tools/pollmove`), marks the kernel tainted (flag 4096), and a card reboot undoes it. `scripts/card.sh stop` restores them, because Intel-Phi-VisionTrack finds its spare core by their old place.
- **The team's stand-down on a still monitor is built and gated, not yet measured live**: how long the team takes to return, and what it saves. No watts are available: the card's board sensors read nothing here and the host has no UPS tool.
- **The region part overlaps vgd's claim** ([1792, 2048) MiB, that project being stopped). `scripts/card.sh` refuses to start beside another card program; nothing stops another from starting after it.
- **`card.sh` refuses by an allow-list of what the card may be running.** `FORCE=1` overrides it.

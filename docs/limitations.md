# Limitations

Stated as measured. See [measurements.md](measurements.md).

- **"60" is the monitor's 59.79 Hz, "30" is 29.895.** The monitor has no 60.00 mode at this size.
- **FSR buys nothing measurable here and costs the most.** 15.1 ms a frame, more than the flow, and a refresh or two of latency, for -.031 dB on real footage and at best +.046 dB on a scene. It is in the chain because the chain was ordered so. `--stages 2` takes it out.
- **FSR is not AMD's here in three ways.** On luma, the change carried to the colours: all three channels would be three times the time. At the picture's own size with no jitter, where AMD's weights do damage: 26.353 dB against 32.678. So the new frame is at least 95 % of each pixel, which is a floor AMD does not have. And on generated pictures only.
- **Deblocking is not AMD's.** AMD ships none: the FidelityFX SDK has no such pass and AMF has an on/off for its hardware encoder. This is H.265's normal luma filter, with its decisions taken line by line rather than over four lines, on each colour, at a fixed strength. Its boundaries come from the vectors, so seams not on the 8 pixel grid are not seen.
- **The half-size form stays closer to the truth on real footage.** 33.860 against 33.072 dB, sharpness .911 against .883 before FSR. It loses everywhere a whole-pixel motion can be exact, the scenes a desktop is made of. `--half` runs it.
- **A finer vector grid did not help.** Cells of 4 pixels cost 10.9 ms and lost .033 dB; with FSR in the chain they do not fit. The grid stays at 8.
- **Small fast things double.** A 120 px box at 90 px a source frame: 9.472 dB, against 10.399 for the plain mean and 10.210 at the half-size field. Not overcome.
- **Uncovered areas ghost.** Where a thing was hidden in one of the two frames and the place is in the picture, the mean of the two has nothing to agree on. AMD's inpainting was not ported.
- **Latency.** With FSR, real use: two groups a refresh apart, 55 to 60 ms and 70 to 75 ms, from the cast's stamp to scanout; 85 to 90 under full test motion. About 45 to 65 without FSR; 38 to 48 at the half-size field. All on top of what the source itself takes. Measured from KWin's stamp and presentation feedback, never with a camera on the glass.
- **The budget is tight with FSR.** 28.6 ms median, a worst frame of 34.70 ms in 13 s, over the 33.45 and recovered from. On a card shared with anything else it would not hold.
- **Refreshes are missed now and then, and the chain seems to raise it.** 25 discards in 129 s of real use with the whole chain, against 49 in 993 s of the half-size form and 0 to 3 in 20 s with no doubler at all, in sessions not interleaved and with different content. Cause not found.
- **Drift.** The cast stamp to scanout rises about .03 ms/s; generated frames are dropped to take it back: 1 in 993 s of real use.
- **The port's picture was never compared against AMD's on a GPU.** Its deviations are in [INDEX.md](INDEX.md).
- **The block pollers' move buys nothing measurable while they are idle.** Under block traffic it was not measured. It takes a one-shot kernel module (`tools/pollmove`), marks the kernel tainted (flag 4096), and a card reboot undoes it. `scripts/card.sh stop` restores them, because Intel-Phi-VisionTrack finds its spare core by their old place.
- **What the stand-down saves is not known in watts.** The card's board sensors read nothing here and the host has no UPS tool. Die temperatures went from 48-53 C to 52-57 C with one team spinning.
- **The region part overlaps vgd's claim** ([1792, 2048) MiB, that project being stopped). `scripts/card.sh` refuses to start beside another card program, by an allow-list that `FORCE=1` overrides; nothing stops another from starting after it.

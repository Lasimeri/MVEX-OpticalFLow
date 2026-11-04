# Index of the AMD optical-flow port

The port lives in `Intel-Phi-VisionTrack` at commit `ac165e8`. This folder holds what doubling a monitor needs of it. The hashes of the copies are in [PROVENANCE.sha256](PROVENANCE.sha256). VisionTrack itself was only read.

Which AMD version: the `_v5` shaders of FSR 3's frame-generation path, `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/` of the FidelityFX SDK, branch `main`, no commit pinned. `ffx_opticalflow.h` has no version macro, so `v5` is the only label. The three shaders were fetched 11-04 into `third_party/opticalflow/`, MIT, `LICENSES/FidelityFX-MIT.txt`.

## What is AMD's and what is not

| Stage | Port (`src/kern/framegen.c`) | AMD shader | Same? |
|---|---|---|---|
| Search, 256 places, 8 x 8 block, radius 8 | `fg_search` | `ComputeOpticalFlowAdvanced`, `compute_optical_flow_v5.h` | Yes in shape. Plain SAD where `msad4` is masked (zero reference bytes ignored). Ties: `abs(dx) + abs(dy)` in Q6 added to the cost, where AMD's optional `FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS` orders the key |
| Zero-motion fallback on level 0 | end of `fg_search` | `blockSadSum <= (minSad >> 16)`, same file | Yes |
| Filter, the 3 x 3 vector nearest the rest | `fg_filter` | `FilterOpticalFlow`, `filter_optical_flow_v5.h` | Yes: same order, same `(sum << 4) \| i` minimum |
| Upscale, best of 4 by SAD over 4 x 4 | `fg_upscale` | `ScaleOpticalFlowAdvanced`, `scale_optical_flow_advanced_v5.h` | Yes: same offsets, same `* 2` |
| Motion off at a scene change | `scene_changed` in search and upscale | `IsSceneChanged()` | Yes in effect; the test is the port's |

Deviations from AMD inside those stages: the field is built at half resolution (AMD's is at full, per the GPUOpen manual), and `FG_ENOUGH` skips the search on level 0 when the estimate already explains the block.

| Stage | Where | Whose |
|---|---|---|
| Scene test: 9 sections, 64 bins, L1 over 0.5 | `fg_hist_section`, `fg_scene_verdict` | The port's, not AMD's divergence |
| The vector at the middle instant | `fg_mid_vectors` | The port's. FSR 3 interpolation leans on a game's motion vectors, which a screen does not have |
| The picture: triangle windows over 9 blocks | `fg_blend` | The port's, grey |
| Colour: BGRx to luma, a picture of 16 BGRA pixels a register | `src/kern/bgra.c` | New here |
| Order of the stages, taken out of the tracker | `src/kern/flow.c` | From `track.c`: `tk_generate_make`, `fg_build_more`, the chunked `OP_FG_*` dispatch, the rule `w >= 16 && h >= 16` for levels |

Not ported from AMD: `compute_luminance_pyramid`, `prepare_luma`, `generate_scd_histogram`, `compute_scd_divergence`.

The port was never compared against AMD's shaders on a GPU. Its agreement with itself on two machines is what is measured: see `measurements.md`.

## Files

| Copied unchanged | From `Intel-Phi-VisionTrack` | Role |
|---|---|---|
| `src/kern/framegen.{c,h}` | same path | The port |
| `src/kern/plane.h`, `img.h`, `img_vx.c`, `img_ref.c` | same path | F32 planes, padding, the pyramid's `img_reduce_vx` |
| `src/common/vx.h`, `vt_util.h` | same path | Every MVEX operation with its host lane model; cycle counter, FNV |
| `src/card/team.{c,h}`, `topo.{c,h}`, `arena.{c,h}`, `vt_dma.{c,h}` | same path | Spinning pinned team, topology, huge pages, the card's own DMA channel |
| `LICENSES/FidelityFX-MIT.txt`, `third_party/opticalflow/ffx_opticalflow.h` | same path | Licence, AMD's interface header |
| `scripts/audit.sh`, `scripts/compdb.sh`, `config.mk` | same path | ISA audit, clangd check, tool paths. Adapted, not identical: names and paths |

| Left out on purpose | Why |
|---|---|
| `fsr3.*`, `third_party/fsr3` | An upscaler, not interpolation |
| `track.c` | The tracker. The flow's order was taken out of it |
| `vx_gather.S`, `depth`, `stereo`, `kalman`, `lk_*`, `detect`, `vecdraw` | Nothing here reaches them: a `-Werror` build with an audit found the closure |

| New here | Role |
|---|---|
| `src/common/vxu.h` | MVEX integer operations, in the form of `vx.h` |
| `src/common/mf_proto.h` | Host and card contract, region part [1920, 1984) MiB |
| `src/kern/bgra.{c,h}`, `flow.{c,h}` | Colour; the pipeline |
| `src/card/mvexflowd.c` | The card's program |
| `src/host/mvexflow.c`, `wlp.c`, `pwc.c`, `mf_link.c`, `mf_quit.c` | The host's program: Wayland overlay, PipeWire cast, link |
| `src/host/mf_replay.c` | The gate |
| `tools/probe.c`, `tools/moving.c` | Design probe; test motion for the monitor |
| `tools/pollmove/` | One-shot kernel module moving the block pollers |
| `tools/protocols/` | Wayland protocol XML, absent from the system |
| `scripts/card.sh`, `pollers.sh`, `excl.sh`, `mvexflow.sh`, `live_test.sh` | Card daemon, pollers, capture exclusion, the program, the bounded live test |

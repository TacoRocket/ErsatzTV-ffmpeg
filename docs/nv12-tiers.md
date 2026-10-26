# NV12 output tiers for libplacebo tone mapping on VAAPI

ErsatzTV's VAAPI tone-mapping chain currently has libplacebo write an RGBA
picture, then runs a second GPU pass (`scale_vulkan=format=nv12`) to turn it
into the NV12 frame the encoder needs:

```
hwmap=derive_device=drm,format=drm_prime,
libplacebo=tonemapping=...:colorspace=bt709:color_primaries=bt709:color_trc=bt709:format=rgba,
scale_vulkan=format=nv12:out_range=limited,
hwmap=derive_device=vaapi:reverse=1,format=vaapi
```

The tiers below remove that detour. Each tier states two things: how fast it
is, and how its picture compares with Tier 0.

| Tier | What it is | Picture vs Tier 0 |
| --- | --- | --- |
| 0. RGBA | The current chain above | Reference |
| 1. Native | libplacebo's own NV12 output (Mitchell chroma filter), written directly | libplacebo's original picture, not Tier 0's |
| 2. Match | One pass that reproduces Tier 0's conversion | Same bytes |
| 3. Match + delayed peak (HDR), dropped | Tier 2, with HDR peak detection allowed to use the previous frame's reading | Differs on real content, visibly at scene cuts |

## Enabling them

Tiers 1-3 render into the VAAPI NV12 surface directly:

```
hwmap=derive_device=drm,format=drm_prime,
libplacebo=tonemapping=...:colorspace=bt709:color_primaries=bt709:color_trc=bt709:format=nv12:range=tv,
hwmap=derive_device=vaapi:reverse=1,format=vaapi
```

| Tier | Environment | Extra libplacebo option |
| --- | --- | --- |
| 1 | `ETV_VAAPI_DIRECT_NV12=1 ETV_INLINE_NV12=native` | |
| 2 | `ETV_VAAPI_DIRECT_NV12=1 ETV_INLINE_NV12=match` | |
| 3 (experiment only) | `ETV_VAAPI_DIRECT_NV12=1 ETV_INLINE_NV12=match` | `allow_delayed_peak=1` (experiment patch 9002) |

`ETV_VAAPI_DIRECT_NV12=1` (FFmpeg patch 0020) makes the mapped NV12 surface
writable by Vulkan. Without `ETV_INLINE_NV12`, libplacebo renders that target
with its stock multi-pass planar output.

## Picture proof

All comparisons are raw NV12 (`hwdownload,format=nv12`) checked with `cmp`.

| Check | Frames | Differing bytes |
| --- | ---: | ---: |
| Match vs RGBA, 1920x1080 fixture | 24 | 0 |
| Match vs RGBA, 1440x1080 pillarbox fixture | 24 | 0 |
| Match vs RGBA, 1920x800 letterbox fixture | 24 | 0 |
| Match vs RGBA, 1280x720 | 24 | 0 |
| Match vs RGBA, I Am Groot S01E01, HDR10 2160p via `scale_vaapi` | 120 | 0 |
| Match vs RGBA, I Am Groot S02E01, Dolby Vision 8.1 2160p via `scale_vaapi` | 120 | 0 |
| Native vs previous producer-storage output, three 1080p layouts | 24 each | 0 |

Tier 3 was compared with Tier 2 frame by frame (FFmpeg `psnr`) on the
encoded HLS output of the five-stream timing runs below, where the GPU runs
behind. On the static fixtures all 7,200 frames per layout matched. On I Am
Groot S01E01, 4,376 of 5,760 frames differed. In one 30-second stream, 173
frames were identical, 502 differed by 40 dB PSNR or more, 46 by 30-40 dB and
6, around scene cuts, by less than 30 dB (worst 22.5 dB). The control pair
(Tier 2 with and without the PR79 handoff, identical input pixels) differed on
0 of 25,200 frames, so the encoder is deterministic and the differences come
from the delayed peak. Tier 3 gained 1-2% (table below) and is dropped.

How Match gets the same bytes (patch `0001-nv12-direct-output-match.patch`):

1. RGB is left unencoded and dithered to 8 bits exactly as the RGBA output is
   dithered, so each pixel holds the texel value the RGBA route writes.
2. Those values are read back through a 256-entry UNORM8 ramp texture with a
   linear sampler, the conversion `scale_vulkan`'s `texture()` read applies.
3. `scale_vulkan`'s BT.709 matrix (`ff_fill_rgb2yuv_table`), limited-range
   scale and offset and top-left chroma placement follow, unchanged.
4. Y and UV are stored with `imageStore`, as `scale_vulkan` stores them. The
   libplacebo pass keeps its shader stage.

Steps 2 and 4 reuse the GPU's own conversions instead of imitating them: the
GPU's UNORM8 reads and writes are not exactly k/255, and exact float math
alone missed about 0.1% of Y bytes by one code value. Forcing the pass from
fragment to compute changed 72-432 bytes per 24 frames, so the stage is kept.

## Speed

Five concurrent streams x 720 frames (30 s), Hable, 1080p24 limited BT.709,
H.264 VAAPI 5 Mbps, AAC, HLS; slowest FFmpeg runtime, mean of two batches in
reversed order. Every route used the same FFmpeg build (8.1.2 with this
repository's patches 0001-0020 plus experiment patches 9001/9002, which leave
the reference bytes unchanged) and the same libplacebo (v7.360.1 + 0001 +
0002). Only route flags differ.

| Layout | Tier 0 RGBA | Tier 1 Native | Tier 2 Match | Tier 3 (dropped) |
| --- | ---: | ---: | ---: | ---: |
| 1920x1080 | 27.512 s | 27.279 s (+0.9%) | 23.398 s (+17.6%) | 23.178 s (+18.7%) |
| 1440x1080, x=240 | 21.807 s | 21.720 s (+0.4%) | 19.210 s (+13.5%) | 18.809 s (+15.9%) |
| 1920x800, y=140 | 21.713 s | 21.318 s (+1.9%) | 18.703 s (+16.1%) | 18.544 s (+17.1%) |

Real content: I Am Groot S01E01 (HEVC 2160p HDR10) through ErsatzTV next's
VAAPI order for a 4K source (`scale_vaapi=1920:1080` first, then libplacebo).
Four concurrent streams (five exceeded the test container's 5 GB memory
limit):

| Tier | Seconds per 30 s | Speed | vs Tier 0 |
| --- | ---: | ---: | ---: |
| 0 RGBA | 30.736 s | 0.98x | |
| 1 Native | 31.814 s | 0.94x | -3.4% |
| 2 Match | 28.742 s | 1.04x | +6.9% |

At four 4K HDR streams Tier 0 falls just short of real time and Tier 2 does
not. The gain is smaller than on the fixtures because 4K decode loads the
video engine, which this change does not touch.

Three Groot streams paced at real time (`-re`, 60 s; every stream held
0.998x), mean GPU engine busy over the middle 60% of the run:

| Tier | Render/3D | Video |
| --- | ---: | ---: |
| 0 RGBA | 71.0% | 64.6% |
| 1 Native | 71.6% | 64.3% |
| 2 Match | 62.4% | 62.0% |
| 3 (dropped) | 62.8% | 62.8% |

Same streams, same picture, about 12% less render-engine time.

## PR79 (async handoff and two command slots)

Tiers 1-2 need the writable NV12 target from patch 0020 (PR78's part).
They do not need PR79's async reverse handoff and two-slot command pool.
Experiment patch 9001 adds `ETV_VAAPI_NV12_ASYNC=0`, which keeps the target
and turns the handoff and slots off. Match without them was 1.4-1.7 points
faster on the three fixtures and within 0.4% on Groot; on the RGBA route they
measured -3.5% / +0.9% earlier. The two upstream commits PR79 adapts ship in
FFmpeg 9.0 (`25e187f84949`) and on master for 9.1 (`1d14f9f8c3e4`).

## Limits and claims

- Byte identity and timings are from one Intel i3-1220P (Gen12, Mesa ANV).
  Both routes run on the same GPU and share its read and store conversions;
  what does not cancel is how the compiler builds the two shaders.
- A compile-only check with Mesa's Intel drm-shim and the same Mesa (25.2.8)
  compiled both routes for Skylake, Ice Lake, Tiger Lake, Alder Lake, DG2,
  Meteor Lake, Lunar Lake, Battlemage and Panther Lake. The RGB->YUV and the
  tone-mapping/dither arithmetic compile to the same operations in the same
  order in both routes on every generation; the compiler's fusion rules do
  not vary by generation or shader stage.
- One difference is not arithmetic: `scale_vulkan` stores chroma from all
  four pixels of each 2x2 block, and the last write wins. On the tested Gen12
  that is the top-left pixel, the one Match uses; Gen9-Gen12 dispatch that
  compute shader in the same linear thread order. From Gen12.5 (DG2 onward)
  Mesa uses a tiled order, so which write lands last there is unverified.
- Expected therefore: luma identical on Gen9-Xe3, chroma identical on
  Gen9-Gen12, Gen12.5+ chroma unverified. Results are pinned to Mesa 25.2.8;
  the check reruns in one container for another Mesa version.
- Native assumes the fp16 attachment truncation measured on Intel ANV and is
  gated to 1920x1080. Match makes no rounding assumption and accepts any even
  size with even-aligned bars.
- Match covers limited-range BT.709 NV12, the range of `scale_vulkan`'s
  `out_range=limited`. A full-range target falls back to libplacebo's original
  output (verified byte-identical to it).
- Unsupported layouts, hooks or capabilities keep the original libplacebo
  output path.
- The test libplacebo was built without libdovi, so the Dolby Vision episode
  ran on its HDR10 base layer in both routes.

# Direct NV12 output trial

> **Branch `perf/nv12-match-base`: libplacebo patches reorganized.** The
> pinned libplacebo build now applies two patches, selected at runtime with
> `ETV_INLINE_NV12`:
>
> - `0001-nv12-direct-output-match.patch` (`ETV_INLINE_NV12=match`): direct
>   NV12 output byte-identical to `format=rgba` + `scale_vulkan`, one pass.
> - `0002-nv12-direct-output-native.patch` (`ETV_INLINE_NV12=native`):
>   libplacebo's own NV12 output (Mitchell chroma) with producer storage, the
>   former 0001/0002 behavior, including its Intel ANV rounding assumption.
>
> `ETV_INLINE_NV12_RTZ` and `ETV_INLINE_NV12_CHROMA` and the point mode are
> gone on this branch. The sections below record how the work got here and
> still name the earlier patch files and variables.

This fork carries an opt-in Linux VAAPI/Vulkan path for rendering into an
NV12 VAAPI surface. It preserves the existing synchronous route unless
`ETV_VAAPI_DIRECT_NV12=1` is set. The patch supplies writable attachment usage,
separate R8/RG8 plane metadata, a timeline-to-sync-file handoff, and two command
slots. Failed sync-file handoff falls back to the existing CPU wait.

The timeline bridge follows FFmpeg `25e187f84949`; pool depth follows
`1d14f9f8c3e4efb32000cb463ebb635ad6f2adb0`. These are adaptations to FFmpeg 8.
They are not credited as new upstream work or independent speed wins.

The amd64 Docker build accepts `DIRECT_NV12_OUTPUT=1`. This builds the pinned
libplacebo v7.360.1 commit with `patches/libplacebo/0001-opt-in-nv12-producer-storage.patch`,
followed by `0002-nv12-padded-output.patch`,
`0003-nv12-single-pass-point-chroma.patch` and `0004-nv12-single-pass-match-rgba-route.patch`.
The ordinary dependency image and other build targets are unchanged. Use the
normal pinned dependency image and release build arguments, plus
`--build-arg DIRECT_NV12_OUTPUT=1 --build-arg BUILD_JOBS=3`.

At runtime, `ETV_INLINE_NV12_RTZ=1` separately enables the producer storage
optimization. It writes Y during the existing color producer and retains UV in
RG16F scratch. Chroma filtering and dithering remain in their original order.
Unsupported layouts, backends and enabled-feature combinations retain the
original libplacebo output path; dispatch/allocation failures remain failures.
Both options are off by default. They do not alter the application's shader.

**Rounding limit:** the opt-in assumes the fp16 attachment truncation observed
on Intel ANV. Vulkan does not guarantee this behavior on every driver. The RTZ
name makes that assumption explicit; there is no runtime calibration or device
database. Do not enable it on another driver expecting bit parity. The current
eligibility gate covers 1920x1080 8-bit BT.709 SDR NV12 output, either full-frame
or an even-aligned picture rectangle surrounded by bars, after libplacebo's
normalization and coordinate rounding. Output hooks, overlays,
rotation, blending, distortion and error diffusion retain the original path,
as do rectangles whose rounded coordinates are odd-aligned and translated
white-noise dithering. Originally fractional requests can qualify if their
normalized, rounded rectangle is even-aligned.
It checks enabled fragment image-store support and RG16F storage capability.
Other output sizes use the original path. Broader portability remains open.

A controlled five-stream HLS replay on an Intel i3-1220P integrated GPU measured
15.235 s for RGBA, 16.5535 s for point-sampled direct NV12, and 14.814 s with
producer storage, for 360 frames per stream. That is a 2.84% throughput gain
against RGBA and a 10.51% completion-time reduction against direct without the
storage change. The shader was unchanged; matching point chroma is a caller
choice, not a new default. Two repetitions and one machine do not establish a
universal gain. Rebuilt-fork verification is recorded separately before adoption.

The experimental comparison checked 37 HLS outputs / 15,480 decoded frames.
Storage on/off outputs matched. It was not a live application/client test.
Five streams were below real time in that CRT workload. HDR output, 10-bit
output and cross-driver claims beyond the documented trials are unverified.

## Padded tone-mapping checkpoint

The second libplacebo patch retains the same producer-storage saving when a
picture has black bars. For example, 1440x1080 content inside a 1920x1080 frame
needs its Y writes shifted 240 pixels to the right. Dither lookup coordinates
move by the same amount to preserve quantized pixels. Bar clearing and both
Mitchell chroma filters remain unchanged. The existing libplacebo dither code
is reused through a private helper; its public API is unchanged.

An isolated replay used the public HDR HEVC fixtures and Hable tone-mapping
chain from [ErsatzTV/next at 5df6ccbf](https://github.com/ErsatzTV/next/tree/5df6ccbf2f5610079c8ac20deb158eb86279e4f8/crates/ffpipeline/tests).
Output was 1080p24, limited-range BT.709, H.264 VAAPI at 5 Mbps, AAC and HLS.
Five workers each encoded 720 frames. Two rounds reversed route order. The
table reports the mean slowest FFmpeg runtime per batch, including decode,
tone mapping, handoff, encoding and muxing; it excludes container startup.

| Layout | RGBA | Direct before padding patch | Direct with padding patch |
| --- | ---: | ---: | ---: |
| 1440x1080, x=240 | 22.2575 s | 24.9385 s | 22.3770 s |
| 1920x800, y=140 | 22.2770 s | 24.4370 s | 21.7720 s |

This is 11.45% and 12.24% more throughput than the direct fallback, respectively.
The small differences versus RGBA (-0.53% and +2.32%) establish competitiveness
in these runs, not universal superiority. Input HDR tone mapping was exercised;
the output remains SDR. No point-sampling or `skip_aa` change was used.

All 144 raw frames across full-frame/pillarbox/letterbox controls and candidates
matched byte-for-byte. The 60 encoded outputs formed four exact segment-byte
groups: RGBA and direct for each padded layout. Candidate and direct-control
segments matched across all workers and repetitions. Each distinct output was
fully decoded to 720 frames without errors. That represents 43,200 encoded
frames through deduplication, not 60 independent complete decodes.

This evidence is from one Intel i3-1220P/ANV system and synthetic fixtures. It is
an FFmpeg-chain replay, not execution of the Rust test suite or a live client
acceptance result. The two libplacebo libraries were built with the same release
options using glslang; a new full `DIRECT_NV12_OUTPUT=1` Docker build with shaderc
has not been completed for this checkpoint. Default CI does not enable that
optional build. The inherited half-float rounding limitation and the separate
software-Vulkan rounding investigation remain; this patch does not claim to fix
them. Disabling `ETV_INLINE_NV12_RTZ` retains the original output path.

## Single-pass NV12 checkpoint (point chroma)

The third libplacebo patch adds a speed mode. With `ETV_INLINE_NV12_RTZ=1` and
`ETV_INLINE_NV12_CHROMA=point`, the producer pass renders Y straight into the
destination Y plane and stores UV from the top-left sample of each 2x2 block.
That is the chroma placement FFmpeg's `scale_vulkan=format=nv12` already uses in
the current RGBA route. No half-float scratch image and no separate plane passes
remain, so the RGBA write plus `scale_vulkan` conversion is replaced by zero
extra passes. Bars, padding offsets and dither origin follow patch 0002. Without
the chroma opt-in, the 0001/0002 behavior (Mitchell chroma) is unchanged, and
unsupported layouts keep the original output path.

Picture check, same fixtures and chain as above, 8 raw NV12 frames per layout:

| Layout | Y vs RGBA route | UV vs RGBA route | Y vs Mitchell direct |
| --- | --- | --- | --- |
| 1920x1080 | max 2, mean 0.23 | max 5, mean 0.15 | max 1, mean 0.04 |
| 1440x1080, x=240 | max 1, mean 0.17 | max 2, mean 0.20 | max 1, mean 0.03 |
| 1920x800, y=140 | max 2, mean 0.21 | max 5, mean 0.18 | max 1, mean 0.03 |

Bars are exactly Y=16, UV=128 and picture edges line up with the RGBA route.
Single-pass is not byte-identical to RGBA: the RGBA route quantizes RGB to 8 bits
before converting, single-pass converts at full precision and dithers once.

Timing for point is in the match section's table below.

Limits: point chroma matches the current RGBA route's chroma, not Mitchell; it is
the speed mode, and Mitchell remains the quality mode. One Intel i3-1220P/ANV,
synthetic fixtures, libplacebo built with glslang (the optional Docker build uses
shaderc and was not rebuilt for this checkpoint).

## Match mode: one pass, byte-identical to the RGBA route

Plainly: the current ErsatzTV chain has libplacebo write an RGBA picture, then a
second GPU pass (`scale_vulkan=format=nv12:out_range=limited`) reads that picture
back and converts it to NV12. Match mode performs the same conversion inside
libplacebo's own pass, with the same arithmetic and the same GPU read and store
paths, so the NV12 bytes are the same and the RGBA image plus second pass go away.

Enable it with the direct NV12 setup above (`ETV_VAAPI_DIRECT_NV12=1`,
`libplacebo=...:format=nv12:range=tv`) plus `ETV_INLINE_NV12_RTZ=1` and
`ETV_INLINE_NV12_CHROMA=match`. Patch `0004-nv12-single-pass-match-rgba-route.patch`
applies after 0001-0003.

What the pass does, in order:

1. Leaves RGB unencoded and dithers it to 8 bits exactly as the RGBA output is
   dithered, so each pixel holds the texel value the RGBA route writes.
2. Reads those values back through a 256-entry UNORM8 ramp texture with a linear
   sampler, the conversion `scale_vulkan`'s `texture()` read applies.
3. Applies `scale_vulkan`'s BT.709 matrix (`ff_fill_rgb2yuv_table`, as a dynamic
   mat4), limited-range scale and offset, and top-left UV placement, unchanged.
4. Stores Y and UV with `imageStore`, the store `scale_vulkan` uses. A fragment
   producer keeps its stage, stores Y itself and discards into an unwritten
   scratch attachment; a compute producer's own store is used.

Picture proof, 24 raw NV12 frames per layout, same binary and fixtures, compared
with `cmp` against the RGBA route (`hwdownload,format=nv12` after each chain):

| Layout | Differing bytes | NV12 sha256 (both routes) |
| --- | ---: | --- |
| 1920x1080 | 0 | `85295d54fef4596b...` |
| 1440x1080 at x=240 | 0 | `c691825dde4f3576...` |
| 1920x800 at y=140 | 0 | `9c1c599796cfcd91...` |

Why each step is there (measured, not assumed):

- An offline model of `scale_vulkan` from libplacebo's captured RGBA output
  confirmed BT.709 coefficients and top-left chroma. Exact float math still
  missed about 0.1% of Y bytes by one code value: Intel's UNORM8 read and store
  conversions are not exactly k/255. Steps 2 and 4 use those hardware paths
  instead of emulating them.
- Forcing the producer to compute left 72-432 differing bytes per 24 frames
  (one code value, fixed pixels): the RGB itself differed before the dither, so
  the compiled upstream math changed with the stage. Keeping the stage removed it.

Timing, same five-worker x 720-frame harness (Hable, 1080p24 limited BT.709, H.264
VAAPI 5 Mbps, AAC, HLS), slowest FFmpeg runtime, mean of two batches in reversed
order. All four routes used the same FFmpeg and the same libplacebo build
(0001-0004); only the route flags differed:

| Layout | RGBA + `scale_vulkan` | Native (Mitchell) | point | match | match vs RGBA |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920x1080 | 26.397 s | 27.226 s | 23.155 s | 23.594 s | +11.9% |
| 1440x1080, x=240 | 22.985 s | 22.905 s | 19.047 s | 19.331 s | +18.9% |
| 1920x800, y=140 | 22.791 s | 21.276 s | 18.236 s | 18.797 s | +21.2% |

The RGBA route varied 5-8% between batches; every match batch was faster than
every RGBA batch. Exact rounding costs match about 2% against point. With
`ETV_VAAPI_DIRECT_NV12=1` on the RGBA route as well (async handoff and two
slots, no NV12 target), RGBA changed by -3.5% (full) and +0.9% (letterbox), so
the single pass, not the handoff, carries the gain.

Which mode to use:

| Mode | Picture | Work after tone mapping | Role |
| --- | --- | --- | --- |
| `match` (0004) | Same bytes as RGBA + `scale_vulkan` on this Intel GPU | none | Drop-in replacement for the current route |
| `point` (0003) | Rounds once instead of twice; within 1-2 Y and 2-5 UV codes of RGBA | none | Alternative fast mode |
| Native (0001/0002) | libplacebo's own output, Mitchell chroma filter | scratch image + 2 passes | Original libplacebo picture, for purists |

Limits: byte identity is shown on one Intel i3-1220P with ANV. On another driver
the math is the same but the read/store conversions or compiled upstream math
may differ; treat it as point-like there until checked. Eligibility is the 0001
gate (1920x1080 8-bit BT.709 SDR, full frame or even-aligned bars); otherwise the
deferred RGB encode is restored and the original output path runs. libplacebo
was built with glslang and FFmpeg's `scale_vulkan` came from the image's shaderc
build; the optional Docker build (shaderc for both) was not rebuilt here.

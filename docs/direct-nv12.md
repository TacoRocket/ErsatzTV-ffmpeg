# Direct NV12 output trial

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
followed by `0002-nv12-padded-output.patch`.
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
or an even-aligned picture rectangle surrounded by bars. Output hooks, overlays,
rotation, blending, distortion and error diffusion retain the original path,
as do fractional/odd-aligned rectangles and translated white-noise dithering.
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

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
libplacebo v7.360.1 commit with `patches/libplacebo/0001-opt-in-nv12-producer-storage.patch`.
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
eligibility gate covers full-frame 1920x1080 8-bit BT.709 SDR NV12 without output
hooks, overlays, rotation, blending or error diffusion. It checks enabled
fragment image-store support and RG16F storage capability. Other geometry uses
the original output path. Broader portability remains open.

A controlled five-stream HLS replay on an Intel i3-1220P integrated GPU measured
15.235 s for RGBA, 16.5535 s for point-sampled direct NV12, and 14.814 s with
producer storage, for 360 frames per stream. That is a 2.84% throughput gain
against RGBA and a 10.51% completion-time reduction against direct without the
storage change. The shader was unchanged; matching point chroma is a caller
choice, not a new default. Two repetitions and one machine do not establish a
universal gain. Rebuilt-fork verification is recorded separately before adoption.

The experimental comparison checked 37 HLS outputs / 15,480 decoded frames.
Storage on/off outputs matched. It was not a live application/client test.
Five streams were below real time. Hardware/API, HDR, 10-bit and cross-driver
claims beyond the documented trial are unverified.

# VAAPI / OpenCL picture investigation — 2026-10-26

Fork-only diagnostic checkpoint, based on `13a4f05` and FFmpeg 8.1.2.
**The fixed-Hable hardware LUT probe works; normal OpenCL's scene-adaptive
picture remains unmatched.** These patches are outside the release patch
directory and are not applied by the normal build.

The intended outcome is to retain the lower general-render usage of VAAPI HDR
tone mapping while matching the existing OpenCL Hable picture. VAAPI instead
produces substantially darker pixels on the tested HDR10 material. The relevant
boundary is FFmpeg's tone-mapping filters and the Intel driver's implementation,
not the H264 encoder, NV12 shader optimization, or player. Patch 0026's metadata
fallback remains unchanged; the initial diagnostic controls below did not fix
the picture. The subsequently authorized hardware LUT probe is recorded below.

## Initial picture-attribution method

- Intel i3-1220P, iHD 26.1.5, libva 2.22.0 (reported VA API 1.23).
- One 3840x2160 24 fps HDR10 HEVC input; BT.2020/PQ, mastering peak 1000 nits,
  MaxCLL/MaxFALL both zero (unknown).
- VAAPI decode, `scale_vaapi=1920:1080:extra_hw_frames=64:force_divisible_by=2:mode=default`,
  `setsar=1`, then either tone mapper. No CRT shader.
- OpenCL: `tonemap_opencl=tonemap=hable:desat=0:peak=10:t=bt709:m=bt709:p=bt709:format=nv12`
  between the existing VAAPI/OpenCL hardware maps.
- VAAPI: `tonemap_vaapi=format=nv12:t=bt709:m=bt709:p=bt709`.
- Seek to 60, 120 and 180 seconds; filter 49 frames and retain frame 48, allowing
  two seconds of OpenCL scene-history warmup. Download raw NV12 before encoding.
- Measure mean Y in the common central rectangle x=0..1919, y=140..939, excluding
  the letterbox. Numbers are 8-bit code values, **not nits or perceptual scores**.
  Every successful capture is exactly 3,110,400 bytes.
- Playback was idle for the captures. These initial controls did not produce a
  matching candidate and were not timed. The subsequent hardware LUT probe below
  includes its own resource comparison.

## What accounts for the difference

OpenCL's `peak=10` does not disable scene analysis. Its kernel detects peak and
average brightness, then uses those values in the Hable transform. A diagnostic
replaced that detection result with `{peak, sdr_avg}` while leaving the rest of
the OpenCL transform unchanged. This is an attribution probe, not a proposed
change to the production OpenCL picture.

| Seek / sampled frame | Normal OpenCL | Fixed-peak OpenCL | VAAPI |
|---|---:|---:|---:|
| 60 s / frame 48 | 70.60 | 53.57 | 37.18 |
| 120 s / frame 48 | 67.12 | 55.46 | 39.47 |
| 180 s / frame 48 | 55.90 | 45.43 | 31.23 |

Removing OpenCL's scene adjustment reduces the gap, but a 14–16 Y-code mean
difference remains between fixed Hable and VAAPI. Thus scene adjustment accounts
for part of the observed gap; a difference in the remaining color/tone conversion
also remains. This does **not** identify the precise driver formula responsible
or prove either rendering is the universally correct one. The different color
conversion and output transfer functions have not been isolated from the curve.

The difference exists in raw pixels before encoding. Matching BT.709 tags alone
cannot fix it. A tone-map-before-scale control at 60 seconds gives mean Y=37.27
instead of 37.18; reversing these stages does not explain the large brightness gap.

## Controls that did not work

- `peak-control.patch` overrides the submitted input MaxCLL, preserving zero as
  the original behavior. Values 100, 400 and 1000 nits produced byte-identical
  VAAPI output at all three seeks. Instrumented repeats at 100 and 4000 nits
  confirmed those actual values in the mapped VAAPI parameter buffer at 60 s;
  those images also matched the original VAAPI bytes.
- Explicit SDR target metadata at 100 and 200 nits, using the existing `display`
  option with explicit BT.709 output, produced the same bytes at 60 s. This option
  also attaches mastering metadata to output and is not a proposed SDR deployment
  setting. No new target-brightness option was added.
- Changing output transfer from BT.709 to sRGB produced the same bytes at 60 s.

Intel's code substitutes 4000 nits when input MaxCLL is unknown, but the failed
overrides mean **we have not established that default as the cause**. Instrumentation
confirms FFmpeg's submission, not the driver's internal use of every field. The
libva trace did not expose the HDR payload sufficiently to extend that claim.

## Practical next mechanism

`vaapi_caps.c` queries the standard VAAPI video-processing capability interface;
it does not submit frames. It compiled with `-Wall -Wextra` and returned 3D LUT
support for 17, 33 and 65 points per dimension, 16-bit entries, four channels,
and channel mapping mask 7. See `caps.txt` for the exact advertised strides.

A hardware 3D lookup table is a credible way to apply a chosen RGB color/tone
transform on video hardware. Capability advertisement alone does not prove pixel
accuracy, performance, successful surface import, or matching OpenCL's changing
scene adjustment. The first checkpoint (`6c5de15`) implemented no LUT surface.
Neither the checked FFmpeg source nor Jellyfin's source exposed
`VAProcFilter3DLUT`; this would need an adapter rather than a setting change.

The operator then approved the bounded fixed-Hable probe below. Exact
moving-picture parity with normal OpenCL additionally needs compatible scene
analysis, which was not part of the fixed-curve probe authorization. The operator
clarified that maintainability chiefly means routine container deployment and
upgrades: a self-contained feature with its assets in the normal build, without
manual host setup or a separate rendering service. This is not a blanket rejection
of scene adaptation if it can meet that operational boundary. Main keeps OpenCL
while packaging and the remaining picture difference are discussed.

## Hardware lookup-table probe

`hardware-lut.patch` adds an optional `probe_lut` file argument to the existing
`tonemap_vaapi` filter. With it unset, the filter follows the original path.
When explicitly set, it queries the standard VAAPI 3D LUT capability, checks the
65/65/128 layout, uploads a 4,326,400-byte table once to a linear surface, and
submits that table through the existing filter/render lifecycle. The table
replaces the driver's HDR transform for this probe; it is not a second tone
mapper layered after it. Decode, scaling and encoding remain unchanged.

The adapter is about 140 added C lines including lifecycle plumbing. The
46-line NumPy generator runs offline, not per frame. It samples the fixed
1000-nit Hable transform, including PQ linearization, BT.2020-to-BT.709 gamut
conversion and the same BT.1886 output transfer as OpenCL. There is no scene
analysis, custom driver, per-frame CPU download, or new runtime dependency.
This is deliberately a fixed-layout experiment, not a portable general LUT API.
Invalid/missing LUT data or unavailable capability aborts the explicit probe;
it does not silently switch algorithms and invalidate the comparison.

The upload sequence follows Intel's existing libva-utils example. See
[THIRD_PARTY.md](THIRD_PARTY.md) for credit and licenses. The generated table
is reproducible data; no media or generated binary is committed.

### Picture

All three sampled hardware-LUT frames completed. The full-frame average absolute
luma difference from **fixed** OpenCL Hable is 0.325, 0.347 and 0.207 code values.
Chroma-plane averages are 0.69–0.90 code values. The largest observed luma
difference is 8; the largest chroma difference is 16 in the 120-second scene
(0.77% of its V samples differ by more than four). This is close in these frames,
not byte identity or proof for all HDR sources. The first paired stills were also
visually inspected. `lut-metrics.json` preserves the full statistics and hashes.

The original VAAPI route with the new option **unset** matches the original
0026 control byte-for-byte at the 60-second capture. Both the fixed-OpenCL and
LUT HLS outputs decoded all 720 frames without errors. All measured encodes
selected the low-power VAAPI H264 entrypoint.

### Resource and speed results

Same 4K HDR10 input, 1080p24 output, 5 Mbps H264 low-power encoder, AAC and HLS.
Three paced streams, 1080 frames per stream, engine means over the middle 60%:

| Route | Render/3D busy | Video busy | VideoEnhance busy |
|---|---:|---:|---:|
| Normal OpenCL Hable | 54.25% | 52.65% | 34.50% |
| Fixed OpenCL Hable | 53.30% | 57.92% | 37.99% |
| Fixed Hable hardware LUT | 1.69% | 50.08% | 47.11% |

All completed at the requested playback rate. Fixed OpenCL and the LUT are the
comparable fixed-transform pair; normal OpenCL is a current-behavior resource
reference with a different adaptive picture. These are engine-busy observations,
not power measurements. Background services were left alone; the guard checked
live Tunarr and the other known FFmpeg test container for conflicting encodes.

Four unpaced streams, 720 frames per stream, with order reversed in the second
pair. Rates use the slowest stream's FFmpeg wall time:

| Pair | Fixed OpenCL | Hardware LUT | LUT improvement |
|---|---:|---:|---:|
| OpenCL then LUT | 27.94 fps / 1.164x | 31.32 fps / 1.305x | 12.1% |
| LUT then OpenCL | 28.46 fps / 1.186x | 31.48 fps / 1.312x | 10.6% |

This supports a modest throughput benefit and a large shift away from general
render for this fixed-transform workload. Video busy approached 99% during the
unpaced LUT runs. It is not a repeat of the older CRT/NV12 comparison or proof
of a specific additional stream count.

An initial fixed-OpenCL paced run overlapped resumed live playback and was
discarded. The guard detected it, but cleanup initially missed the diagnostic
binary's different process name. The owned test container was stopped, the
sampler was stopped, and cleanup was corrected to use the selected binary name.
The fixed control and both speed pairs were rerun after playback exited.

### Disposition and reproduction

Keep this as an optional fork experiment. The fixed-transform resource benefit
is demonstrated, but the container-upgrade maintenance goal is not yet proven:
the probe and generated asset have not been integrated into the normal image
recipe. Any deployable version should include its executable support and assets
in that recipe and expose an explicit selection, with no extra service or manual
host preparation. It is **not** a deployment-ready replacement for normal
scene-adaptive OpenCL. Further adaptation and packaging remain separate work;
this checkpoint does not authorize a general LUT framework or deployment.

Apply `hardware-lut.patch` directly after fork patches 0001–0026, without
`peak-control.patch`. Rebuild FFmpeg, run `python3 make_lut.py` with NumPy available,
and append `:probe_lut=/absolute/path/hable65.bin` to the VAAPI filter above.
The original unmodified binary and the separate `static-opencl.patch` binary
remain the controls. Use the identity table only as an upload diagnostic.

## Reproduction and ownership

The patches apply to FFmpeg 8.1.2 after this fork's patches 0001–0026. Apply each
only to a disposable source tree with `patch -p1`; rebuild FFmpeg after changing
the OpenCL kernel because it is embedded in the binary. Preserve an unmodified
0026 binary as the control. `static-opencl.patch` is diagnostic only and must not
be confused with the normal OpenCL control in speed or picture claims.

Compile the capability query with:

```sh
cc -Wall -Wextra -O2 vaapi_caps.c $(pkg-config --cflags --libs libva libva-drm) -o vaapi_caps
./vaapi_caps
```

`pixel-metrics.json` retains per-capture hashes and measurements. Private run
commands, logs, exact binary identities and media locations remain in the
deployment companion. No media images or clips are added to the fork. This is
an investigation checkpoint, not a new release, PR, or deployed tone mapper.

Primary sources:

- [FFmpeg OpenCL tone mapping](https://github.com/FFmpeg/FFmpeg/blob/n8.1.2/libavfilter/opencl/tonemap.cl)
- [Intel HDR defaults](https://github.com/intel/media-driver/blob/intel-media-26.1.5/media_common/agnostic/common/vp/hal/vp_common_hdr.h)
- [Intel HDR input handling](https://github.com/intel/media-driver/blob/intel-media-26.1.5/media_softlet/linux/common/vp/ddi/ddi_vp_functions.cpp)
- [Intel HDR pipeline selection](https://github.com/intel/media-driver/blob/intel-media-26.1.5/media_softlet/agnostic/common/vp/hal/feature_manager/sw_filter.cpp)
- [VAAPI 3D LUT interface](https://github.com/intel/libva/blob/2.22.0/va/va_vpp.h)

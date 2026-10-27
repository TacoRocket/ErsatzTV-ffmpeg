# VAAPI / OpenCL picture investigation — 2026-10-26

Fork-only diagnostic checkpoint, based on `13a4f05` and FFmpeg 8.1.2.
**No picture fix is validated. These patches are outside the release patch
directory and are not applied by the normal build.**

The intended outcome is to retain the lower general-render usage of VAAPI HDR
tone mapping while matching the existing OpenCL Hable picture. VAAPI instead
produces substantially darker pixels on the tested HDR10 material. The relevant
boundary is FFmpeg's tone-mapping filters and the Intel driver's implementation,
not the H264 encoder, NV12 shader optimization, or player. Patch 0026's metadata
fallback remains unchanged; the diagnostic controls below did not fix the picture.

## Method

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
- Playback was idle for the captures. No new timed utilization comparison was
  made because none of the picture controls produced a matching candidate.

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
scene adjustment. No LUT surface or transform was implemented in this checkpoint.
Neither the checked FFmpeg source nor Jellyfin's source exposed
`VAProcFilter3DLUT`; this would need an adapter rather than a setting change.

The proposed next bounded probe is fixed Hable through that interface, compared
against the fixed OpenCL diagnostic for picture and against normal OpenCL for
resource use. Exact moving-picture parity with normal OpenCL additionally needs
compatible scene analysis; that is a separate decision, not an assumed free step.
Keep the live OpenCL route while deciding whether to pursue that adapter.

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

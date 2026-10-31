# VAAPI tone-mapping metadata fallback

Fork-only experiment on `fix/vaapi-hdr-metadata`, based on the existing
`perf/nv12-native-layer` checkpoint `eae0935`. No upstream PR or deployment.

## Failure and correction

HDR10 playback should continue when the input stream supplies static mastering
metadata but an individual frame does not repeat it. On the tested HEVC/Matroska
input, `tonemap_vaapi` instead stopped after 80 frames (3.333 seconds). Starting
at 60 seconds failed before encoding video. Both reported
`No mastering display data from input`.

Patch 0034 changes only `libavfilter/vf_tonemap_vaapi.c`. It reads each frame's
mastering-display and content-light metadata first, then uses the corresponding
input filter-link metadata when that frame lacks it. It does not invent metadata
or add a previous-frame metadata cache as a substitute for stream metadata. The
existing error remains when neither location supplies mastering metadata.

For HDR-to-SDR conversion it also removes inherited mastering/content-light
metadata from the output link, matching the filter's existing per-frame cleanup.
Without that cleanup the SDR raw output still advertised the input's 1000-nit
mastering display. The pixel calculation, driver parameters, and HDR-to-HDR
output-metadata policy are otherwise unchanged.

This is separate from the NV12 libplacebo patches. It does not change those
shaders, storage formats, dithering, encoder settings, or Tunarr configuration.

## Upstream check — 2026-10-26

No merged correction for this failure was found:

- [FFmpeg at d31915e](https://github.com/FFmpeg/FFmpeg/blob/d31915e9dce4d33676fadc6c3e08965b4acc132b/libavfilter/vf_tonemap_vaapi.c)
  still requires per-frame mastering metadata. Its reader has no link fallback.
- ErsatzTV's checked main patch set, through 0022, has no correction for this
  reader. The AV1 QSV HDR patch fixes a different duplicate-side-data failure.
- Jellyfin's corresponding filter also has the per-frame requirement. Its
  existing SDR metadata removal concerns output, not the missing input case.

The related pending FFmpeg color-negotiation proposal does not supply missing
mastering metadata. This lookup is a dated source check, not a claim that no
unpublished or unlocated fix exists.

## Isolated validation

Both control and candidate were built from the checksum-verified FFmpeg 8.1.2
tarball with the then-current fork patches 0001-0025 (before the 8.1.2-7 sync).
Only the candidate adds the metadata fix, now numbered 0034. The build
enables the VAAPI/OpenCL decode/filter/encode path, rather than all optional
features of the distributed image. This is not a rebuilt production image or
a portability matrix.

The direct replay uses a 3840x2160 24 fps HDR10 HEVC source, explicit-default
VAAPI scaling to 1920x1080, VAAPI HDR-to-SDR conversion, H264 VAAPI low-power
encoding at 5 Mbps, AAC, and four-second HLS segments.

| Check | Control | Candidate |
|---|---|---|
| Start at zero, request 720 frames | Fails after 80 | 720/720 |
| Seek to 60 seconds | Fails before video output | 720/720 |
| First 48 raw NV12 frames, where control succeeds | Reference | All frame hashes identical |
| No mastering metadata on frame or stream | Existing missing-metadata error | Same error; no fabricated metadata |
| Decode both candidate HLS outputs | — | 720 frames each, no video decode errors |
| Output metadata | Raw SDR output inherits HDR mastering/light data | Limited BT.709, inherited HDR mastering/light data removed |

Frame-specific metadata still takes precedence by inspection of the code; no
fixture with conflicting frame/link metadata was exercised. HDR-to-HDR and
non-Intel drivers remain untested. The input's intermittent per-frame metadata
was reproduced with software decode too; this patch does not change the decoder
or establish why every source authoring path omits that metadata.

Driver VAAPI tone mapping and OpenCL Hable are different picture algorithms.
Successful decoding and identical first-frame output against the old VAAPI
implementation do not establish visual parity with OpenCL. Operator picture
acceptance and a complete packaged-runtime check remain before adoption. The
inspected VAAPI scene is visibly darker than the corresponding OpenCL output.
The missing-metadata negative check uses an SDR output, relabeled PQ and converted
to P010, without adding mastering data. An initial synthetic-source attempt was
invalid because this reduced build lacks the wrapped-frame decoder; it was not
counted as a passing check.

## Resource comparison, not picture equivalence

The same patched probe binary also completed the previously blocked comparison.
On the Intel i3-1220P, four unpaced 720-frame jobs per route, followed by a reversed
order, produced these per-stream rates using the slowest job's completion time:

| Route | First run | Reversed run |
|---|---:|---:|
| OpenCL Hable | 25.51 fps / 1.063x | 28.18 fps / 1.174x |
| Driver VAAPI | 27.96 fps / 1.165x | 28.19 fps / 1.174x |

One pair favors VAAPI by 9.6%; the other is effectively tied. This does not prove
a repeatable maximum-throughput improvement. At three paced 24 fps streams,
both completed 1080 frames per stream. Middle-60%-of-run engine samples were:

| Route | Render/3D busy | Video busy | VideoEnhance busy |
|---|---:|---:|---:|
| OpenCL Hable | 51.62% | 54.27% | 34.87% |
| Driver VAAPI | 9.00% | 56.56% | 50.59% |

That is a measured shift away from general render work in this scene, not a
power measurement or proof of additional stream capacity. The two tone-mapping
algorithms differ in picture, so this is not a same-image speed optimization.
It is also separate from the earlier default-scaling and CRT/NV12 measurements.

The subsequent [isolated picture investigation](../experiments/vaapi-tonemap-picture/README.md)
separates OpenCL scene adjustment from the remaining conversion difference.
Its optional hardware LUT probe closely matches fixed Hable with lower render
use. Normal scene-adaptive OpenCL remains unmatched; no change is deployed.

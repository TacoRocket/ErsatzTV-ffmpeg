# Shared-memory Mitchell chroma probe

This is an experimental patch for the shared `perf/nv12-padded-output` branch.
It is not applied by the Dockerfile and is not a change to upstream PR80.
The tested padding checkpoint is `6b65ed2`. The separate point-sampling candidate
arrived at `1036af7`; this probe instead retains Mitchell filtering.

The existing direct path runs vertical chroma filtering, writes an intermediate
image, then reads it for horizontal filtering. This probe uses tiled separable
convolution: a GPU workgroup computes vertical results into shared memory, then
uses those results for horizontal filtering in the same dispatch. It retains
the existing filter weights, half-float intermediate quantization and final
dithering. The earlier producer pass still writes Y and full-resolution UV
scratch; this is not a one-pass conversion of the whole output.

## Measured result

Full-frame public HDR fixture from ErsatzTV/next `5df6ccbf`, Hable tone mapping
to 1080p24 limited-range BT.709, H.264 VAAPI 5 Mbps, AAC and HLS. Five concurrent
workers each encoded 720 frames. Same binary, library, input and encoder settings;
only route/probe flags changed. Two batches per route, reversing order.

| Route | Batch seconds | Mean seconds | Playback speed per stream |
| --- | --- | ---: | ---: |
| RGBA plus scale_vulkan | 26.171 / 26.010 | 26.0905 | 1.1498x |
| Existing Mitchell NV12 | 26.957 / 27.061 | 27.0090 | 1.1107x |
| Shared-memory Mitchell | 26.016 / 26.003 | 26.0095 | 1.1534x |

Seconds are the slowest FFmpeg runtime per batch. Speed is 30 seconds of media
divided by that runtime, not bytes/sec. The probe measured 3.84% more throughput
than existing Mitchell and 0.31% above RGBA. Treat the latter as a tie, not a
substantiated RGBA lead. This is one Intel i3-1220P/ANV system and synthetic input.
Other test/playback activity was absent in 151 sampled observations; background
services were unchanged. This does not compare against the separate point mode.

A separate 240-frame trace, discarding the first 20 shader samples, measured
0.626 ms vertical plus 0.316 ms horizontal for the old output filters, versus
0.737 ms for the combined pass. GPU intervals are not an exclusive, additive
end-to-end time budget. The full-pipeline timings above establish the net gain.

## Picture limit and disposition

Across 24 raw frames, all 49,766,400 Y bytes matched the control. 118,104 of
24,883,200 UV bytes differed (0.475%), each by one 8-bit code value. Thus the
probe retains the filtering algorithm but does not yet preserve exact output.
Explicitly decoding packed half-float values to prevent compiler folding left
the result unchanged; that known rounding issue does not explain this difference.
Sampling coordinates/interpolation and arithmetic ordering remain possible causes,
not established findings. The disabled probe matches the prior padding control
hash exactly. The 30 encoded outputs form three byte-identical groups; one full
720-frame representative of each group decoded without errors.

Keep this as an unpromoted research checkpoint. Further work would need to
explain the one-level residual before claiming picture-exact Mitchell output.
The picture-exact padding extension remains independently useful.

## Reproduction boundary

The patch applies after 0001/0002 in `patches/libplacebo`. Application after the
point patch 0003 was also checked, but that combined source was not rebuilt here.
The measured library used libplacebo v7.360.1, glslang, release settings, Vulkan
enabled and OpenGL disabled; the Docker shaderc build remains unverified.

Use `ETV_VAAPI_DIRECT_NV12=1`, `ETV_INLINE_NV12_RTZ=1`, and leave
`ETV_INLINE_NV12_CHROMA` unset. Toggle `ETV_NV12_SHARED_CHROMA_PROBE=0/1` against
the same library. The probe deliberately recognizes only the observed 1920x1080
RG16F to 961x540 left-sited sampling rectangle and eight-tap Mitchell filter.
Unsupported cases use the existing sampler. Allocation/dispatch failures remain
visible failures. It inherits the Intel attachment-rounding assumption and has
not been tested on another driver. Generalizing dimensions or padding is deferred.

The filter generation, LUT and shader infrastructure belong to libplacebo;
this patch changes where intermediate results are held. It is not a new filter.
Source/library hashes and compact measurements are in `results.json`. Removing
this experiment directory removes the checkpoint artifacts; no shipping build
currently applies this patch.

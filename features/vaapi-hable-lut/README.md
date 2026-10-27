# Optional VAAPI fixed-Hable tone mapper

This module implements a **3D lookup table (3D LUT)** through VAAPI's video
processing API. It converts limited-range P010 BT.2020 NCL/PQ into limited-range
BT.709 NV12. The table uses fixed 1000-nit Hable, a BT.2020-to-BT.709 gamut matrix,
and FFmpeg's BT.1886 output math. It does **not** perform OpenCL's scene adaptation.
It is an optional alternative, not a pixel-identical replacement for normal OpenCL.

`vf_tonemap_vaapi_lut.c` owns the filter and LUT surface lifecycle. It reuses
FFmpeg's `vaapi_vpp` device, frame and submission code. `register.patch` only
registers the source and checks for the libva 3D-LUT type. The stock VAAPI and
OpenCL filters are unchanged by this module. `make_lut.py` generates the table
using Python's standard library and checks its SHA256 against the accepted probe.
The runtime needs neither Python nor an external asset mount or service.

## Build and select

The Linux amd64 Docker recipe defaults to `VAAPI_HABLE_LUT=0`. Add
`--build-arg VAAPI_HABLE_LUT=1` to the normal build described in
[building.md](../../docs/building.md). Keep `--build-arg DIRECT_NV12_OUTPUT=1`
when building the existing direct-NV12 runtime. The two features are independent.

The image contains `tonemap_vaapi_lut` and
`/usr/local/share/ffmpeg/luts/hable-1000-65.bin`. Select it explicitly in a VAAPI
graph, for example `scale_vaapi=w=1920:h=1080:mode=default,tonemap_vaapi_lut`.
Applications can detect the filter through `ffmpeg -filters`. Building it does
not change any existing graph or default tone mapper.

Our companion Tunarr change selects it with `TUNARR_VAAPI_HABLE_LUT=1` only for
known 10-bit, limited-range BT.2020 NCL/PQ input not identified as Dolby Vision.
Unset/0 keeps the existing preference (OpenCL by default). Other input formats
keep the existing path. Missing FFmpeg support logs a warning and uses the
configured tone mapper. A driver/layout or asset failure after selecting the
filter is an explicit FFmpeg error, not a silent picture substitution.

## Upgrade boundary

1. Apply the normal fork patches to the pinned FFmpeg release.
2. The image runs `install.sh`: check/apply the small registration patch, copy
   the filter source, generate the LUT and bundle its attribution.
3. Configure/build normally. Enabled builds must expose the new filter or fail.
4. Run the normal package checks, then an isolated hardware HDR10 decode ->
   scale -> LUT -> low-power encode and decode the result. Compare a raw frame
   with the accepted probe, and replay the default OpenCL graph unchanged.
5. Stage the combined application image with the runtime flag off before any
   operator-approved cutover. Disable the flag to return to normal tone mapping;
   use the previous immutable image if the package itself needs rollback.

No generic plugin loader, driver patch or runtime table generator is involved.
On upgrades the owned seams are `configure`, `libavfilter/Makefile`,
`libavfilter/allfilters.c`, and the shared VPP API used by this one source file.
If one changes incompatibly, the build fails at that seam. Remove this folder
and its Docker block to retire the feature; it has no stored configuration schema.

## Supported scope and proof

Requires a driver advertising a 65/65/128, 16-bit, four-channel RGB LUT and a
contiguous linear RGBA storage surface. These are checked at initialization.
No GPU vendor is hardcoded; **hardware validation is currently Intel Gen12 only**.
The C filter rejects other input encodings and removes HDR metadata from its SDR
frames and output link. It does not infer missing color tags or apply Dolby Vision
metadata. Fixed peak and gamut handling have the same limitations as the probe.

The original isolated probe's picture and resource results are in
[the experiment](../../experiments/vaapi-tonemap-picture/README.md). Those numbers
are not new measurements of this package. Package validation is checkpointed
separately. Intel libva-utils and FFmpeg attribution is in [THIRD_PARTY.md](THIRD_PARTY.md)
and is included in the image beside the generated asset.

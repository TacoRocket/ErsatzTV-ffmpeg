# Optional VAAPI fixed-Hable tone mapper

This module implements a **3D lookup table (3D LUT)** through VAAPI's video
processing API. It converts limited-range P010 BT.2020 NCL/PQ into limited-range
BT.709 NV12. The table uses fixed 500-nit Hable, a BT.2020-to-BT.709 gamut matrix,
and FFmpeg's BT.1886 output math. It does **not** perform OpenCL's scene adaptation.
It is an optional alternative, not a pixel-identical replacement for normal OpenCL.

`vf_tonemap_vaapi_lut.c` owns the filter and LUT surface lifecycle. It reuses
FFmpeg's `vaapi_vpp` device, frame and submission code. `register.patch` only
registers the source and checks for the libva 3D-LUT type. The stock VAAPI and
OpenCL filters are unchanged by this module. `make_lut.py` generates the table
using Python's standard library and checks its SHA256 against the reviewed table.
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

## Table brightness

The curve reaches full output at 500 nits (`PEAK_NITS` in `make_lut.py`). It was
1000 at first, because the table was built to copy the OpenCL setting `peak=10`
with its scene detection removed. That copy came out darker than a mapper that
adapts to each scene, and no other value had been tried.

A fixed table has one trade to make. Output levels spent on highlights are not
available for everything else, so a lower peak gives a brighter picture and less
room above it. Anything brighter than the peak comes out at full level.

Measured on one Alder Lake-P machine with the packaged filter, only the table
file changed. 45 stills at 1280x720 from 15 clips of 4K HDR10 and Dolby Vision
film: dark, ordinary, one very bright title (MaxCLL 9918), and three titles
chosen for strong colour. The reference is libplacebo `tonemapping=linear` with
peak detection and the Dolby Vision data ignored. Brightness is the mean luma
difference as a share of the reference's mean luma above black, letterbox excluded.

| Peak | Mean | Stills more than 10% darker | Stills more than 10% brighter | Range |
|---|---:|---:|---:|---|
| 1000 | -10% | 29 | 3 | -21% to +16% |
| 500 | -1% | 7 | 9 | -13% to +27% |
| 400 | +3% | 0 | 11 | -10% to +32% |

- **Highlights.** With the sun in frame the reference puts 2.7% of the picture
  within ten steps of white; 500 puts none there and 400 puts 1.7%.
- **Bright, saturated colour.** Every fixed table is paler than the reference in
  these areas. On the pastel title the loss is 20 to 24% at 1000, 12 to 16% at
  500 and 9 to 13% at 400, so a lower peak is the less washed one.
- **Colour pushed to its limit.** In the neon still where the tables add the most,
  1.7% of the picture has a colour channel at maximum at 500, against 0.9% for the
  reference and 5.6% at 400.

500 sits nearest the reference on average and keeps more room for neon colour
than 400. It was judged on stills, not in motion. The picture results in
[the experiment](../../experiments/vaapi-tonemap-picture/README.md) describe the
1000 table and are not this package's picture.

The 500-nit table keeps the filename `hable-1000-65.bin` for compatibility
with existing installations and bind mounts.

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

### Input height

On Intel Gen12 the video-enhance engine (VEBOX) can hang when its 3D LUT runs on
an input whose height is not a multiple of 8. The kernel logs
`GPU HANG ... in vf#0:0` on `vecs0` and resets the context; FFmpeg then fails
with `HW busy` or stops producing frames. Whether a given clip hangs depends on
its content, so a height can pass on one title and hang on another.

Intel's driver knows the defect: iHD forces the VEBOX input height to a multiple
of 8 for 3D LUT work (`Wa_16021308716` in `vp_scaling_filter.cpp`), but only when
its scaler runs in the same pass and only for some heights. This filter converts
without scaling, so that workaround never applies to it.

The filter therefore never gives the LUT such a height:

- **Height is a multiple of 8** (2160, 1080, 800, 720, 544, 480, 360): one pass,
  as before. Surfaces and output are unchanged.
- **Any other height** (1084, 804, 540, 404): the picture is first copied,
  unscaled, into the top of a surface whose height is the next multiple of 8.
  The LUT then runs on that surface. The output frame keeps the picture height;
  its surface is the taller one, as decoder output is for a cropped stream. The
  extra rows are black and are never part of the frame.

Callers need no change and the output bytes are the same as before for every
height that did not hang. The copy is one extra VPP pass on those heights only;
on the test machine it cost about 5% of uncapped speed at 1920x804 and up to 20%
at 960x540. A caller that can choose its sizes avoids it by handing the filter a
height that is a multiple of 8, for example by padding before the filter instead
of after. The copy uses its own VA context: sharing one context with the LUT
pass produced pictures without the LUT applied.

Measured on one Alder Lake-P machine (iHD 26.1.5, i915 with GuC): heights with a
remainder of 2 or 4 hung on every clip tried, sooner or later; a remainder of 6
did not hang in six runs and a remainder of 0 never did. All non-multiples are
treated the same.

The original isolated probe's picture and resource results are in
[the experiment](../../experiments/vaapi-tonemap-picture/README.md). Those numbers
are not new measurements of this package. Package validation is checkpointed
separately. Intel libva-utils and FFmpeg attribution is in [THIRD_PARTY.md](THIRD_PARTY.md)
and is included in the image beside the generated asset.

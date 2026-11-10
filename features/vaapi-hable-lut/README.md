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

## Probe: per-scene tables for Dolby Vision (`scene=1`)

**Status: probe. Off by default, not packaged, not for release.**

The fixed table assumes every scene peaks at 1000 nits, so most scenes come out
darker than the libplacebo path shows them. For Dolby Vision, libplacebo's
`linear` curve takes one number per frame: the level 1 maximum from the Dolby
metadata, with a floor of 100 nits. That number steps at shot cuts and is flat
between them.

With `tonemap_vaapi_lut=scene=1` the filter reads the same number from each
frame and runs the 3D LUT with a table made for it:

- **Table pack.** `luts/scene-linear-65.bin` holds 26 tables, one per peak from
  100 to 10000 nits. Each was sampled from libplacebo, not computed, so it
  carries libplacebo's curve and gamut mapping as they are.
- **Blend.** For a peak between two pack entries the filter blends the two
  tables. The curve is a straight stretch in PQ, so the weight is linear in
  `1 / PQ(peak)`.
- **Swap.** Three LUT surfaces are kept. A new peak writes its table into the
  least recently used one; a peak seen recently reuses its surface.
- **Fail open.** `scene=0`, a missing or malformed pack, a frame without level 1
  metadata, or a failed table write all give the fixed table.

Build the pack on a machine whose FFmpeg has libplacebo and Vulkan:

```sh
python3 probe/scene_pack.py grid grid.gbrp
for peak in $(python3 probe/scene_pack.py ladder); do
  ffmpeg -f rawvideo -pix_fmt gbrp12le -s 4225x65 -r 1 -i grid.gbrp -frames:v 1 \
    -c:v libx265 -color_range pc -x265-params \
    "lossless=1:master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(${peak}0000,1):max-cll=${peak},$((peak / 4))" \
    grid-$peak.mkv
  ffmpeg -init_hw_device vulkan=vk -filter_hw_device vk -i grid-$peak.mkv -frames:v 1 -vf \
    "setparams=color_primaries=bt2020:color_trc=smpte2084,libplacebo=tonemapping=linear:colorspace=bt709:color_primaries=bt709:color_trc=bt709:peak_detect=0:contrast_recovery=0:dithering=-1:format=gbrp16le" \
    -f rawvideo -pix_fmt gbrp16le tables/x-$peak.gbrp16
done
python3 probe/scene_pack.py pack tables scene-linear-65.bin
```

Install it as `/usr/local/share/ffmpeg/luts/scene-linear-65.bin` (43 MB).

Measured on one Alder Lake-P machine against the libplacebo chain it replaces,
five Dolby Vision clips (four profile 8.1, one profile 7), 4K sources:

- The filter's per-frame peak equals libplacebo's on every frame of all five
  clips, and changes on the same frame.
- 148 frames at 960x540, including the three frames around each of 30 shot cuts:
  mean luma difference 0.34 of an 8-bit step, worst frame 0.74. The fixed table
  differs by 1 to 9 steps on the same clips.
- 4K to 1080p into `h264_vaapi`: 4.5x with the fixed table, 4.5x with scene
  tables, 3.5x to 3.75x with libplacebo. A table takes 2 to 10 ms to build and
  is built once per new peak.

Not done: packaging or generating the pack in the build; sources without Dolby
metadata (libplacebo measures those frame by frame); libplacebo's contrast
recovery, which a table cannot do; profile 5; any GPU but the one above; long
runs and real channel playback.

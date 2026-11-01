# Native NV12 output (optional, on top of Match)

Native keeps libplacebo's own NV12 output: its chroma filter (Mitchell) and
picture are unchanged. Instead of rendering a full-resolution intermediate
first, it writes Y during the color pass and keeps UV in RG16F scratch for
libplacebo's existing chroma passes. Select it with
`libplacebo=...:format=nv12:range=tv:nv12_output=native` and
`ETV_VAAPI_DIRECT_NV12=1`.

What it is for: the original libplacebo picture written straight into the
VAAPI NV12 surface, for anyone who prefers libplacebo's chroma filter to the
RGBA route's top-left chroma. What it is not: faster on the HDR tone-mapping
chain. On the i3-1220P it measured +0.4% to +1.9% against the RGBA route on
three 1080p fixtures and -3.4% with four 2160p HDR10 streams. Its measured
gain was on a custom-shader workload (CRT effects), where libplacebo's
intermediate is the larger cost.

Limits, stated in the patch: it assumes the fp16 attachment truncation
measured on Intel ANV (Vulkan does not promise that rounding elsewhere), and
requires compatible NV12 planes, even-aligned picture rectangles, and writable
GPU storage. The output follows the requested dimensions; there is no resolution
allowlist. Unsupported layouts and capabilities keep the original output path.

A bounded CRT comparison against ordinary NV12 output matched all 24 decoded
frames for each full-frame, pillarbox and letterbox case at 1280x720, 1920x1080
and 3840x2160. Logs confirmed that Native ran in all nine cases. An odd horizontal
offset selected the fallback and matched another 24 frames. These checks establish
picture parity on the tested Intel runtime, not other-driver compatibility, HDR
parity at these sizes, or a speed improvement.

Patches: `patches/libplacebo/0002-nv12-direct-output-native.patch`, applied
after `0001-nv12-direct-output-match.patch`, adds `PL_NV12_OUTPUT_NATIVE`
(marked by `PL_RENDERER_NV12_OUTPUT_NATIVE`). It is additive: one function,
one enum value and the plane-loop changes that let libplacebo's chroma
passes read the scratch image. FFmpeg patch 0033 adds the option value
`native`, compiled in only when that marker is present.

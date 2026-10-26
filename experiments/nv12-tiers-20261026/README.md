# NV12 tier measurements, 2026-10-26

Evidence behind `docs/nv12-tiers.md`. Nothing here is applied by the
Dockerfile.

- `9001-avutil-vulkan-split-async-handoff.patch` (apply after 0020):
  `ETV_VAAPI_NV12_ASYNC=0` keeps the writable NV12 target but restores the
  synchronous reverse wait and the default compute exec pool size. Used to
  measure Match without PR79's handoff and two slots.
- `9002-avfilter-vf_libplacebo-allow-delayed-peak.patch`: libplacebo filter
  option `allow_delayed_peak` setting `pl_peak_detect_params.allow_delayed`.
  Used for Tier 3, which was dropped.
- `results.json`: timing per layout and route (slowest FFmpeg runtime per
  batch), real-time GPU engine busy, the raw NV12 hashes shared by the RGBA
  and Match routes, and build identities.

Method: one Intel i3-1220P (Gen12, Mesa ANV 25.2.8). Five (fixtures) or four
(I Am Groot 2160p) concurrent FFmpeg processes, 720 frames each, two batches
per route in reversed order, other GPU work absent. FFmpeg was a lean 8.1.2
build with this repository's patches 0001-0020 plus 9001/9002; its raw NV12
output matched the image FFmpeg's on the reference captures. Fixtures are
from ErsatzTV/next `5df6ccbf` (`1080p_hevc_10_hdr*.mkv`).

The compile-only cross-generation check (Mesa drm-shim, Skylake through
Panther Lake) is summarized in `docs/nv12-tiers.md`; its logs stay with the
test host.

Removing this directory removes the evidence only.

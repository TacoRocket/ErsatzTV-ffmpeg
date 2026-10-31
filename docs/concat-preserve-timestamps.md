# Joining sequential video sections without changing their clock

Patch `0027-avfilter-concat-preserve-source-timestamps.patch` adds an opt-in
`preserve_ts=1` option to FFmpeg's concat filter. It joins sequential trims of
one source while preserving source timestamps and frame durations. This lets a
caller defer a credits effect until its section is requested without shifting
video relative to uninterrupted audio.

Ordinary concat expects each segment to start at zero and adds a calculated
segment offset. In the demonstrated credits split, a 300ms source gap was lost
and the following picture shifted by about 334ms. Resetting both trimmed
sections to zero discards the original boundary information.

```text
[video]split[a][b];
[a]trim=end=16[before];
[b]trim=start=16[credits];
[before][credits]concat=n=2:v=1:a=0:preserve_ts=1[out]
```

Keep both branches on the original source clock; do not apply
`setpts=PTS-STARTPTS`. Any background used in the credits branch must start at
the same source timestamp. Keep audio outside this join.

The implementation retains the first input time base, rescales later input
frames to it, skips concat's calculated offsets, and forwards the final input's
EOF timestamp without an offset. It requires exactly one video stream per
segment and no audio streams. The caller owns ordered, non-overlapping sections
of the same source and a suitable common clock. Arbitrary unrelated inputs,
timestamp repair, and hardware-frame propagation are outside this contract.
Default concat behavior is unchanged. Rollback removes the option from callers
before reverting the runtime patch.

The VAAPI caller relies on compatible frames from a single VAAPI device. This
was exercised on an Intel i3-1220P; the patch does not add general hardware-frame
context propagation. Other drivers and hardware platforms remain unverified.

## Validation

`bash patches/tests/concat-preserve-source-timestamps.sh /path/to/ffmpeg`
compares frame content and every output PTS, DTS and duration against an unsplit
synthetic source with the reproduced 300ms gap. It runs to natural EOF, verifies
the old normalized graph loses timing, and checks default equals explicit off.
The prior isolated Linux prototype also matched an approximately 203-second
natural-end sample's video timestamps and uninterrupted AAC packet hashes.
These checks do not establish HLS, subtitle, GPU, physical-player or other-platform
acceptance. The [full linux/amd64 runtime build and shell regression](https://github.com/TacoRocket/ErsatzTV-ffmpeg/actions/runs/41153997801)
passed at `c88534f54dae700f285b9315db587fbfe5bf2e68`, using the normal Dockerfile
with its full codec configuration and both existing rendering build options enabled.

Separate [isolated Tunarr HTTP/HLS validation](https://github.com/TacoRocket/tunarr/pull/96)
used that runtime and Tunarr `2187b84d` on the tested Intel device. It exercised
an SDR fixture across the credits boundary and into the next program, with CRT
on and off. This supplies HLS and VAAPI evidence for that scenario beyond the
shell regression; it does not establish physical TV-client, HDR, other-driver
or other-platform acceptance.

The fork-only `Credit join runtime` workflow is dispatch-only and can be run
manually once present on the default branch. It exports a
seven-day Docker image artifact, its SHA256, the image inspection and source/dependency
identities. Download with `gh run download <run-id>` and verify `SHA256SUMS` before
`docker load`; loading the image does not start or replace a service. It neither
pushes registry images nor publishes releases. The existing shell regression also
runs in normal CI and release validation alongside the sub2video regression,
with the same platform conditions.

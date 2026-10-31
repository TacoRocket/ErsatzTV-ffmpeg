#!/usr/bin/env bash
# Reproduce the lost 300ms gap when sequential source trims are rebased by concat.
set -euo pipefail
ffmpeg=${1:-ffmpeg}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
source="testsrc2=size=32x32:rate=30:duration=2,settb=1/1000,select='not(between(t,0.9,1.199))'"
render() {
    "$ffmpeg" -nostdin -hide_banner -loglevel error -f lavfi -i "$source" \
        -filter_complex "$1" -map '[out]' -c:v rawvideo -enc_time_base 1/1000 \
        -fps_mode passthrough -f framecrc "$2"
}
render '[0:v]null[out]' "$work/source"
render '[0:v]split[a][b];[a]trim=end=1.2[first];[b]trim=start=1.2[last];[first][last]concat=n=2:v=1:a=0:preserve_ts=1[out]' "$work/preserved"
diff -u "$work/source" "$work/preserved"
# The previous graph resets the second branch. Assert this fixture still exposes
# its timing loss, instead of silently passing after an accidental fixture edit.
render '[0:v]split[a][b];[a]trim=end=1.2,setpts=PTS-STARTPTS[first];[b]trim=start=1.2,setpts=PTS-STARTPTS[last];[first][last]concat=n=2:v=1:a=0[out]' "$work/rebased"
if cmp -s "$work/source" "$work/rebased"; then
    echo 'FAIL: fixture no longer reproduces the concat timing loss' >&2
    exit 1
fi
# Explicitly disabling the option is identical to the existing default.
render '[0:v]split[a][b];[a]trim=end=1.2,setpts=PTS-STARTPTS[first];[b]trim=start=1.2,setpts=PTS-STARTPTS[last];[first][last]concat=n=2:v=1:a=0:preserve_ts=0[out]' "$work/disabled"
diff -u "$work/rebased" "$work/disabled"
echo 'PASS: source frame content, PTS, DTS and duration survive the gap and natural end; default equals preserve_ts=0'

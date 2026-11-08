#!/usr/bin/env bash
# Exercise the WebVTT encoder and packet settings with authored ASS events.
set -euo pipefail
ffmpeg=${1:-ffmpeg}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/input.ass" <<'ASS'
[Script Info]
ScriptType: v4.00+
PlayResX: 1000
PlayResY: 500
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&HFFFFFF,&HFFFFFF,&H0,&H0,0,0,0,0,100,100,0,0,1,0,0,8,0,0,0,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\pos(250.5,125)}Upper title
Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\an5\pos(600,375)}Lower title
Dialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,fx,{\move(10,10,30,30)\clip(0,0,100,100)}na
Dialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,fx,{\pos(20,20)\clip(0,0,100,100)\p1}m 1 2 l 3 4
Dialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,,{\i1}English lyric{\i0}
Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,{\an7\pos(100,100)\clip(0,0,200,200)}No
Dialogue: 0,0:00:04.00,0:00:05.00,Default,,0,0,0,,Before{\p1}m 1 2 l 3 4{\p0} after
Dialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,fx,{\pos(100,100)\clip(0,0,200,200)}A full lyric
ASS
cat > "$tmp/expected.vtt" <<'VTT'
WEBVTT

00:01.000 --> 00:02.000 line:25.0%,start position:25.1% align:center
Upper title

00:01.000 --> 00:02.000 line:75.0%,center position:60.0% align:center
Lower title

00:02.000 --> 00:03.000
<i>English lyric</i>

00:03.000 --> 00:04.000 line:20.0%,start position:10.0% align:left
No

00:04.000 --> 00:05.000
Before after

00:05.000 --> 00:06.000 line:20.0%,start position:10.0% align:center
A full lyric
VTT
"$ffmpeg" -nostdin -v error -i "$tmp/input.ass" -c:s webvtt "$tmp/output.vtt"
diff -u "$tmp/expected.vtt" "$tmp/output.vtt"

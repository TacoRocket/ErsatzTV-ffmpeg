#!/bin/sh
# Install into a fresh, already patched FFmpeg tree before configure.
set -eu
feature=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
source_dir=$1
datadir=$2
git -C "$source_dir" apply --check "$feature/register.patch"
git -C "$source_dir" apply "$feature/register.patch"
cp "$feature/vf_tonemap_vaapi_lut.c" "$source_dir/libavfilter/"
mkdir -p "$datadir/luts" "$datadir/licenses/vaapi-hable-lut"
python3 "$feature/make_lut.py" "$datadir/luts/hable-1000-65.bin"
cp "$feature/THIRD_PARTY.md" "$datadir/licenses/vaapi-hable-lut/"

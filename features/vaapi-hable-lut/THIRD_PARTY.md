# Attribution for the optional VAAPI Hable module

The standard VAAPI surface packing and submission sequence in `vf_tonemap_vaapi_lut.c`
are adapted from Intel's [libva-utils vpp3dlut sample](https://github.com/intel/libva-utils/blob/fb62481b3011bb6760cc3f247d45fb10cd61e452/videoprocess/vpp3dlut.cpp).
This module adds the LUT to FFmpeg's existing filter lifecycle and validates the
advertised layout and input size. It does not copy the sample's command-line
application or its frame-upload path. Intel's license follows.

Copyright (c) 2009-2021, Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

The fixed Hable, PQ and BT.1886 math in `make_lut.py` follows
[FFmpeg 8.1.2's OpenCL tone mapper](https://github.com/FFmpeg/FFmpeg/blob/n8.1.2/libavfilter/opencl/tonemap.cl)
and [color conversion functions](https://github.com/FFmpeg/FFmpeg/blob/n8.1.2/libavfilter/opencl/colorspace_common.cl),
which are under LGPL 2.1-or-later. The implementation here is a Python standard-library table
generator with fixed peak 10 and no scene adaptation. It does not replace or
claim authorship of those tone-mapping equations. See FFmpeg's
[LGPL license](https://github.com/FFmpeg/FFmpeg/blob/n8.1.2/COPYING.LGPLv2.1).

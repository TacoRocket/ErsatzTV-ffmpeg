/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
/* Optional fixed-Hable HDR10 transform. LUT surface packing is adapted from
 * Intel libva-utils vpp3dlut.cpp; see the bundled THIRD_PARTY.md. */
#include <math.h>
#include <string.h>
#include <va/va_drmcommon.h>
#include "config.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/file.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "avfilter.h"
#include "filters.h"
#include "vaapi_vpp.h"
#include "video.h"

/* Intel VEBOX can hang the video-enhance engine when the 3D LUT runs on an input
 * whose height is not a multiple of 8. iHD forces that alignment itself only on
 * its scaler path (Wa_16021308716), which this filter does not use, so the filter
 * never hands the LUT such a height. See the module README. */
#define LUT_HEIGHT_ALIGN 8

#define SCENE_PACK       FFMPEG_DATADIR "/luts/scene-linear-65.bin"
#define SCENE_SLOTS      3
#define SCENE_HEADER     24
#define SCENE_TABLE_SIZE ((size_t)65 * 65 * 65 * 3 * 2)

typedef struct HableLUTVAAPIContext {
    VAAPIVPPContext vpp_ctx; /* Must be first for the shared VPP lifecycle. */
    VASurfaceID lut_surface;
    /* P010 surfaces of the aligned height; NULL when the input height is already aligned. */
    AVBufferRef *stage_frames_ref;
    /* The copy into those surfaces has its own context. With the copy and the LUT
     * alternating in one context, iHD 26.1.5 left the LUT out of every picture after
     * the first; the cause inside the driver was not traced. */
    VAContextID stage_context;

    /* PROBE: per-scene tables. Off unless scene=1 and the table pack is installed;
     * any fault falls back to the fixed table. */
    int scene;
    int scene_ready;
    uint8_t *pack;
    size_t pack_size;
    int nb_tables;
    const uint8_t *pack_peaks;
    const uint8_t *pack_tables;
    struct SceneSlot {
        VASurfaceID surface;
        VABufferID buffer;
        int nits;
        int64_t last_used;
    } slots[SCENE_SLOTS];
    int scene_last;
    int64_t scene_clock;
    int64_t stat_frames, stat_scene_frames, stat_changes, stat_builds, stat_build_us;
} HableLUTVAAPIContext;

static double scene_pq_to_nits(double v)
{
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128;
    const double c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    const double p = pow(v, 1 / m2);
    return 10000 * pow(FFMAX(p - c1, 0) / (c2 - c3 * p), 1 / m1);
}

static double scene_nits_to_pq(double nits)
{
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128;
    const double c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    const double y = pow(nits / 10000, m1);
    return pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

static void scene_uninit(AVFilterContext *avctx)
{
    HableLUTVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp = &ctx->vpp_ctx;

    if (ctx->scene && ctx->stat_frames)
        av_log(avctx, AV_LOG_INFO, "scene tables: %"PRId64" of %"PRId64" frames, %"PRId64
               " scene changes, %"PRId64" tables built (%.2f ms each)\n",
               ctx->stat_scene_frames, ctx->stat_frames, ctx->stat_changes, ctx->stat_builds,
               ctx->stat_builds ? ctx->stat_build_us / 1000.0 / ctx->stat_builds : 0.0);
    ctx->stat_frames = ctx->stat_scene_frames = ctx->stat_changes = 0;
    ctx->stat_builds = ctx->stat_build_us = 0;
    for (int i = 0; i < SCENE_SLOTS; i++) {
        if (ctx->slots[i].buffer != VA_INVALID_ID && vpp->hwctx)
            vaDestroyBuffer(vpp->hwctx->display, ctx->slots[i].buffer);
        if (ctx->slots[i].surface != VA_INVALID_ID && vpp->hwctx)
            vaDestroySurfaces(vpp->hwctx->display, &ctx->slots[i].surface, 1);
        ctx->slots[i] = (struct SceneSlot) { VA_INVALID_ID, VA_INVALID_ID, 0, 0 };
    }
    if (ctx->pack)
        av_file_unmap(ctx->pack, ctx->pack_size);
    ctx->pack = NULL;
    ctx->scene_ready = 0;
    ctx->scene_last = 0;
}

static void scene_init(AVFilterContext *avctx)
{
    HableLUTVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp = &ctx->vpp_ctx;
    uint64_t linear_modifier = 0;
    VADRMFormatModifierList modifiers = { .num_modifiers = 1, .modifiers = &linear_modifier };
    VASurfaceAttrib attrs[2] = {
        { .type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_RGBA } },
        { .type = VASurfaceAttribDRMFormatModifiers, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypePointer, .value.p = &modifiers } },
    };
    uint32_t count;

    if (av_file_map(SCENE_PACK, &ctx->pack, &ctx->pack_size, 0, avctx) < 0) {
        ctx->pack = NULL;
        av_log(avctx, AV_LOG_WARNING, "Scene tables are not installed; using the fixed table\n");
        return;
    }
    if (ctx->pack_size < SCENE_HEADER || memcmp(ctx->pack, "ETVSCLUT", 8) ||
        AV_RL32(ctx->pack + 8) != 1 || AV_RL32(ctx->pack + 16) != 65)
        goto fail;
    count = AV_RL32(ctx->pack + 12);
    if (count < 2 || count > 256 ||
        ctx->pack_size != SCENE_HEADER + count * (4 + SCENE_TABLE_SIZE))
        goto fail;
    ctx->nb_tables   = count;
    ctx->pack_peaks  = ctx->pack + SCENE_HEADER;
    ctx->pack_tables = ctx->pack_peaks + 4 * count;
    for (int i = 0; i < SCENE_SLOTS; i++) {
        VAProcFilterParameterBuffer3DLUT param = {
            .type = VAProcFilter3DLUT, .lut_size = 65, .lut_stride = { 65, 65, 128 },
            .bit_depth = 16, .num_channel = 4, .channel_mapping = VA_3DLUT_CHANNEL_RGB_RGB,
        };
        if (vaCreateSurfaces(vpp->hwctx->display, VA_RT_FORMAT_RGB32, 65 * 128, 65 * 2,
                             &ctx->slots[i].surface, 1, attrs, FF_ARRAY_ELEMS(attrs)) != VA_STATUS_SUCCESS) {
            ctx->slots[i].surface = VA_INVALID_ID;
            goto fail;
        }
        param.lut_surface = ctx->slots[i].surface;
        if (vaCreateBuffer(vpp->hwctx->display, vpp->va_context, VAProcFilterParameterBufferType,
                           sizeof(param), 1, &param, &ctx->slots[i].buffer) != VA_STATUS_SUCCESS) {
            ctx->slots[i].buffer = VA_INVALID_ID;
            goto fail;
        }
    }
    ctx->scene_ready = 1;
    av_log(avctx, AV_LOG_VERBOSE, "Scene tables ready: %d tables, %d to %d nits\n", ctx->nb_tables,
           (int)AV_RL32(ctx->pack_peaks), (int)AV_RL32(ctx->pack_peaks + 4 * (count - 1)));
    return;
fail:
    av_log(avctx, AV_LOG_WARNING, "Scene tables could not be set up; using the fixed table\n");
    scene_uninit(avctx);
}

/* Scene peak the current libplacebo path uses for Dolby Vision: level 1 max, floor 100 nits. */
static int scene_frame_nits(const AVFrame *frame)
{
    const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    const AVDOVIDmData *l1;

    if (!sd)
        return 0;
    l1 = av_dovi_find_level((const AVDOVIMetadata *)sd->data, 1);
    if (!l1 || !l1->l1.max_pq)
        return 0;
    return av_clip(lrint(scene_pq_to_nits(l1->l1.max_pq / 4095.0)), 100, 10000);
}

/* Gives the parameter buffer whose table matches this peak, building the table if no slot has it. */
static int scene_select(AVFilterContext *avctx, int nits, VABufferID *buffer)
{
    HableLUTVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp = &ctx->vpp_ctx;
    struct SceneSlot *slot = &ctx->slots[0];
    VAImage image = { .image_id = VA_INVALID_ID };
    const uint16_t *a, *b;
    uint16_t *dst, last[4];
    void *mapped = NULL;
    int64_t start;
    int lo, hi, wb = 0;

    if (nits != ctx->scene_last) {
        ctx->stat_changes++;
        av_log(avctx, AV_LOG_VERBOSE, "scene %d nits from frame %"PRId64"\n", nits, ctx->stat_frames - 1);
    }
    ctx->scene_last = nits;
    for (int i = 0; i < SCENE_SLOTS; i++) {
        if (ctx->slots[i].nits == nits) {
            ctx->slots[i].last_used = ++ctx->scene_clock;
            *buffer = ctx->slots[i].buffer;
            return 0;
        }
        if (ctx->slots[i].last_used < slot->last_used)
            slot = &ctx->slots[i];
    }

    for (hi = 0; hi < ctx->nb_tables - 1 && (int)AV_RL32(ctx->pack_peaks + 4 * hi) < nits; hi++)
        ;
    lo = hi && (int)AV_RL32(ctx->pack_peaks + 4 * hi) > nits ? hi - 1 : hi;
    if (lo != hi) {
        /* The curve is a straight stretch in PQ, so tables blend linearly in 1 / PQ(peak). */
        const double x  = 1 / scene_nits_to_pq(nits);
        const double xl = 1 / scene_nits_to_pq(AV_RL32(ctx->pack_peaks + 4 * lo));
        const double xh = 1 / scene_nits_to_pq(AV_RL32(ctx->pack_peaks + 4 * hi));
        wb = av_clip(lrint((x - xl) / (xh - xl) * 65536), 0, 65536);
    }

    start = av_gettime_relative();
    slot->nits = 0;
    if (vaDeriveImage(vpp->hwctx->display, slot->surface, &image) != VA_STATUS_SUCCESS) {
        image.image_id = VA_INVALID_ID;
        goto fail;
    }
    if (image.format.fourcc != VA_FOURCC_RGBA || image.pitches[0] != 65 * 128 * 4 ||
        image.data_size < image.offsets[0] + (size_t)65 * 128 * 65 * 2 * 4)
        goto fail;
    if (vaMapBuffer(vpp->hwctx->display, image.buf, &mapped) != VA_STATUS_SUCCESS)
        goto fail;
    a   = (const uint16_t *)(ctx->pack_tables + lo * SCENE_TABLE_SIZE);
    b   = (const uint16_t *)(ctx->pack_tables + hi * SCENE_TABLE_SIZE);
    dst = (uint16_t *)((uint8_t *)mapped + image.offsets[0]);
    last[3] = 0;
    for (int rg = 0; rg < 65 * 65; rg++) {
        uint16_t *row = dst + (size_t)rg * 128 * 4;
        for (int k = 0; k < 65; k++) {
            for (int c = 0; c < 3; c++)
                last[c] = (a[c] * (uint64_t)(65536 - wb) + b[c] * (uint64_t)wb + 32768) >> 16;
            memcpy(row + k * 4, last, sizeof(last));
            a += 3;
            b += 3;
        }
        for (int k = 65; k < 128; k++)
            memcpy(row + k * 4, last, sizeof(last));
    }
    vaUnmapBuffer(vpp->hwctx->display, image.buf);
    vaDestroyImage(vpp->hwctx->display, image.image_id);
    slot->nits = nits;
    slot->last_used = ++ctx->scene_clock;
    ctx->stat_builds++;
    ctx->stat_build_us += av_gettime_relative() - start;
    *buffer = slot->buffer;
    return 0;
fail:
    if (image.image_id != VA_INVALID_ID)
        vaDestroyImage(vpp->hwctx->display, image.image_id);
    return AVERROR(EIO);
}

static void lut_pipeline_uninit(AVFilterContext *avctx)
{
    HableLUTVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp = &ctx->vpp_ctx;
    scene_uninit(avctx);
    av_buffer_unref(&ctx->stage_frames_ref);
    if (ctx->stage_context != VA_INVALID_ID && vpp->hwctx) {
        vaDestroyContext(vpp->hwctx->display, ctx->stage_context);
        ctx->stage_context = VA_INVALID_ID;
    }
    if (ctx->lut_surface != VA_INVALID_ID && vpp->hwctx) {
        vaDestroySurfaces(vpp->hwctx->display, &ctx->lut_surface, 1);
        ctx->lut_surface = VA_INVALID_ID;
    }
    ff_vaapi_vpp_pipeline_uninit(avctx);
}

static int lut_build_params(AVFilterContext *avctx)
{
    HableLUTVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp = &ctx->vpp_ctx;
    const unsigned width = 65 * 128, height = 65 * 2;
    const size_t expected = (size_t)width * height * 4;
    VAProcFilterCap3DLUT caps[8] = {0};
    VAProcFilterParameterBuffer3DLUT param = {0};
    unsigned count = FF_ARRAY_ELEMS(caps);
    uint64_t linear_modifier = 0;
    VADRMFormatModifierList modifiers = { .num_modifiers = 1, .modifiers = &linear_modifier };
    VASurfaceAttrib attrs[2] = {
        { .type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_RGBA } },
        { .type = VASurfaceAttribDRMFormatModifiers, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypePointer, .value.p = &modifiers } },
    };
    VAImage image = { .image_id = VA_INVALID_ID };
    VAStatus vas;
    uint8_t *data = NULL;
    size_t size = 0;
    void *mapped = NULL;
    int err, supported = 0;

    vas = vaQueryVideoProcFilterCaps(vpp->hwctx->display, vpp->va_context,
                                    VAProcFilter3DLUT, caps, &count);
    if (vas != VA_STATUS_SUCCESS)
        goto va_error;
    for (unsigned i = 0; i < count; i++) {
        supported |= caps[i].lut_size == 65 && caps[i].lut_stride[0] == 65 &&
                     caps[i].lut_stride[1] == 65 && caps[i].lut_stride[2] == 128 &&
                     caps[i].bit_depth == 16 && caps[i].num_channel == 4 &&
                     (caps[i].channel_mapping & VA_3DLUT_CHANNEL_RGB_RGB);
    }
    if (!supported) {
        av_log(avctx, AV_LOG_ERROR, "Hardware Hable requires the advertised 65/65/128 RGB LUT layout\n");
        return AVERROR(ENOSYS);
    }
    err = av_file_map(FFMPEG_DATADIR "/luts/hable-1000-65.bin", &data, &size, 0, avctx);
    if (err < 0)
        return err;
    if (size != expected) {
        av_log(avctx, AV_LOG_ERROR, "Hardware Hable LUT size is %zu; expected %zu bytes\n", size, expected);
        err = AVERROR(EINVAL);
        goto end;
    }
    vas = vaCreateSurfaces(vpp->hwctx->display, VA_RT_FORMAT_RGB32, width, height,
                           &ctx->lut_surface, 1, attrs, FF_ARRAY_ELEMS(attrs));
    if (vas != VA_STATUS_SUCCESS)
        goto va_error;
    vas = vaSyncSurface(vpp->hwctx->display, ctx->lut_surface);
    if (vas != VA_STATUS_SUCCESS)
        goto va_error;
    vas = vaDeriveImage(vpp->hwctx->display, ctx->lut_surface, &image);
    if (vas != VA_STATUS_SUCCESS)
        goto va_error;
    if (image.format.fourcc != VA_FOURCC_RGBA || image.pitches[0] != width * 4 ||
        image.data_size < image.offsets[0] + expected) {
        av_log(avctx, AV_LOG_ERROR, "Hardware Hable LUT requires a contiguous linear RGBA surface\n");
        err = AVERROR(EINVAL);
        goto end;
    }
    vas = vaMapBuffer(vpp->hwctx->display, image.buf, &mapped);
    if (vas != VA_STATUS_SUCCESS)
        goto va_error;
    memcpy((uint8_t *)mapped + image.offsets[0], data, size);
    vas = vaUnmapBuffer(vpp->hwctx->display, image.buf);
    mapped = NULL;
    if (vas != VA_STATUS_SUCCESS)
        goto va_error;
    vas = vaDestroyImage(vpp->hwctx->display, image.image_id);
    image.image_id = VA_INVALID_ID;
    if (vas != VA_STATUS_SUCCESS)
        goto va_error;
    param.type = VAProcFilter3DLUT;
    param.lut_surface = ctx->lut_surface;
    param.lut_size = 65;
    param.lut_stride[0] = 65;
    param.lut_stride[1] = 65;
    param.lut_stride[2] = 128;
    param.bit_depth = 16;
    param.num_channel = 4;
    param.channel_mapping = VA_3DLUT_CHANNEL_RGB_RGB;
    err = ff_vaapi_vpp_make_param_buffers(avctx, VAProcFilterParameterBufferType,
                                          &param, sizeof(param), 1);
    if (err >= 0)
        av_log(avctx, AV_LOG_VERBOSE, "Loaded fixed-Hable 65-point hardware LUT once (%zu bytes)\n", size);
    if (err >= 0 && ctx->scene)
        scene_init(avctx);
    goto end;

va_error:
    av_log(avctx, AV_LOG_ERROR, "Hardware Hable LUT VAAPI operation failed: %s\n", vaErrorStr(vas));
    err = AVERROR(EIO);
end:
    if (mapped)
        vaUnmapBuffer(vpp->hwctx->display, image.buf);
    if (image.image_id != VA_INVALID_ID)
        vaDestroyImage(vpp->hwctx->display, image.image_id);
    if (data)
        av_file_unmap(data, size);
    return err;
}

static int lut_filter_frame(AVFilterLink *inlink, AVFrame *input)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    HableLUTVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp = &ctx->vpp_ctx;
    AVFrame *output = NULL, *staged = NULL;
    const AVFrame *source = input;
    VABufferID scene_buffer;
    VAProcPipelineParameterBuffer params;
    int err = AVERROR(EINVAL);

    if (input->color_primaries != AVCOL_PRI_BT2020 ||
        input->color_trc != AVCOL_TRC_SMPTE2084 ||
        input->colorspace != AVCOL_SPC_BT2020_NCL ||
        input->color_range != AVCOL_RANGE_MPEG ||
        vpp->input_frames->sw_format != AV_PIX_FMT_P010) {
        av_log(avctx, AV_LOG_ERROR,
               "Fixed-Hable LUT requires limited-range P010 BT.2020 NCL/PQ input.\n");
        goto fail;
    }
    if (vpp->va_context == VA_INVALID_ID)
        goto fail;

    /* The surface may be taller than the picture; the frame keeps the picture height. */
    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    err = av_frame_copy_props(output, input);
    if (err < 0)
        goto fail;
    output->color_primaries = AVCOL_PRI_BT709;
    output->color_trc = AVCOL_TRC_BT709;
    output->colorspace = AVCOL_SPC_BT709;
    output->color_range = AVCOL_RANGE_MPEG;
    av_frame_remove_side_data(output, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    av_frame_remove_side_data(output, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    av_frame_remove_side_data(output, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    av_frame_remove_side_data(output, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    av_frame_remove_side_data(output, AV_FRAME_DATA_DOVI_METADATA);

    if (ctx->stage_frames_ref) {
        /* Copy the picture unscaled into the top of a taller surface; the rows
         * below it are black and never leave the filter. */
        VARectangle picture = { .width = outlink->w, .height = outlink->h };
        const VAContextID lut_context = vpp->va_context;

        staged = av_frame_alloc();
        if (!staged) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        err = av_hwframe_get_buffer(ctx->stage_frames_ref, staged, 0);
        if (err < 0)
            goto fail;
        err = av_frame_copy_props(staged, input);
        if (err < 0)
            goto fail;
        err = ff_vaapi_vpp_init_params(avctx, &params, input, staged);
        if (err < 0)
            goto fail;
        params.output_region = &picture;
        vpp->va_context = ctx->stage_context;
        err = ff_vaapi_vpp_render_picture(avctx, &params, staged);
        vpp->va_context = lut_context;
        if (err < 0)
            goto fail;
        source = staged;
    }

    err = ff_vaapi_vpp_init_params(avctx, &params, source, output);
    if (err < 0)
        goto fail;
    params.filters = vpp->filter_buffers;
    params.num_filters = vpp->nb_filter_buffers;
    ctx->stat_frames++;
    if (ctx->scene_ready) {
        const int nits = scene_frame_nits(input);
        if (nits && scene_select(avctx, nits, &scene_buffer) >= 0) {
            params.filters = &scene_buffer;
            params.num_filters = 1;
            ctx->stat_scene_frames++;
        }
    }
    err = ff_vaapi_vpp_render_picture(avctx, &params, output);
    if (err < 0)
        goto fail;
    av_frame_free(&staged);
    av_frame_free(&input);
    return ff_filter_frame(outlink, output);
fail:
    av_frame_free(&staged);
    av_frame_free(&input);
    av_frame_free(&output);
    return err;
}

static int lut_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink = avctx->inputs[0];
    HableLUTVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp = &ctx->vpp_ctx;
    const int aligned_height = FFALIGN(inlink->h, LUT_HEIGHT_ALIGN);
    VAStatus vas;
    int err;

    /* Output surfaces use the aligned height; the link and its frames keep the
     * picture height, as decoder output does for a cropped stream. */
    vpp->output_width  = inlink->w;
    vpp->output_height = aligned_height;
    err = ff_vaapi_vpp_config_output(outlink);
    if (err < 0)
        return err;
    outlink->h = inlink->h;
    if (aligned_height != inlink->h) {
        AVHWFramesContext *stage;

        ctx->stage_frames_ref = av_hwframe_ctx_alloc(vpp->device_ref);
        if (!ctx->stage_frames_ref)
            return AVERROR(ENOMEM);
        stage = (AVHWFramesContext *)ctx->stage_frames_ref->data;
        stage->format    = AV_PIX_FMT_VAAPI;
        stage->sw_format = AV_PIX_FMT_P010;
        stage->width     = inlink->w;
        stage->height    = aligned_height;
        err = av_hwframe_ctx_init(ctx->stage_frames_ref);
        if (err < 0) {
            av_buffer_unref(&ctx->stage_frames_ref);
            return err;
        }
        vas = vaCreateContext(vpp->hwctx->display, vpp->va_config, inlink->w, aligned_height,
                              VA_PROGRESSIVE, NULL, 0, &ctx->stage_context);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to create the LUT staging context: %s\n", vaErrorStr(vas));
            ctx->stage_context = VA_INVALID_ID;
            av_buffer_unref(&ctx->stage_frames_ref);
            return AVERROR(EIO);
        }
        av_log(avctx, AV_LOG_VERBOSE, "Input height %d is not a multiple of %d; "
               "running the LUT on %d rows\n", inlink->h, LUT_HEIGHT_ALIGN, aligned_height);
    }
    outlink->colorspace = AVCOL_SPC_BT709;
    outlink->color_range = AVCOL_RANGE_MPEG;
    av_frame_side_data_remove(&outlink->side_data, &outlink->nb_side_data,
                             AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    av_frame_side_data_remove(&outlink->side_data, &outlink->nb_side_data,
                             AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    av_frame_side_data_remove(&outlink->side_data, &outlink->nb_side_data,
                             AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    av_frame_side_data_remove(&outlink->side_data, &outlink->nb_side_data,
                             AV_FRAME_DATA_DOVI_RPU_BUFFER);
    av_frame_side_data_remove(&outlink->side_data, &outlink->nb_side_data,
                             AV_FRAME_DATA_DOVI_METADATA);
    return 0;
}

static av_cold int lut_init(AVFilterContext *avctx)
{
    HableLUTVAAPIContext *ctx = avctx->priv;
    ff_vaapi_vpp_ctx_init(avctx);
    ctx->lut_surface = VA_INVALID_ID;
    ctx->stage_context = VA_INVALID_ID;
    for (int i = 0; i < SCENE_SLOTS; i++)
        ctx->slots[i] = (struct SceneSlot) { VA_INVALID_ID, VA_INVALID_ID, 0, 0 };
    ctx->vpp_ctx.pipeline_uninit = lut_pipeline_uninit;
    ctx->vpp_ctx.build_filter_params = lut_build_params;
    ctx->vpp_ctx.output_format = AV_PIX_FMT_NV12;
    return 0;
}

#define OFFSET(x) offsetof(HableLUTVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
static const AVOption tonemap_vaapi_lut_options[] = {
    { "scene", "probe: per-scene tables from Dolby Vision level 1 metadata", OFFSET(scene),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { NULL }
};
AVFILTER_DEFINE_CLASS(tonemap_vaapi_lut);

static const AVFilterPad lut_inputs[] = {
    { .name = "default", .type = AVMEDIA_TYPE_VIDEO,
      .filter_frame = lut_filter_frame, .config_props = ff_vaapi_vpp_config_input },
};
static const AVFilterPad lut_outputs[] = {
    { .name = "default", .type = AVMEDIA_TYPE_VIDEO, .config_props = lut_config_output },
};
const FFFilter ff_vf_tonemap_vaapi_lut = {
    .p.name = "tonemap_vaapi_lut",
    .p.description = NULL_IF_CONFIG_SMALL("Fixed 1000-nit Hable HDR10 to SDR using a VAAPI 3D LUT"),
    .p.priv_class = &tonemap_vaapi_lut_class,
    .priv_size = sizeof(HableLUTVAAPIContext),
    .init = lut_init,
    .uninit = ff_vaapi_vpp_ctx_uninit,
    FILTER_INPUTS(lut_inputs),
    FILTER_OUTPUTS(lut_outputs),
    FILTER_QUERY_FUNC2(ff_vaapi_vpp_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};

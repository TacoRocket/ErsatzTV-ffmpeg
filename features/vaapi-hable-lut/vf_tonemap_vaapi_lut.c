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
#include <string.h>
#include <va/va_drmcommon.h>
#include "config.h"
#include "libavutil/file.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "filters.h"
#include "vaapi_vpp.h"
#include "video.h"

/* Intel VEBOX can hang the video-enhance engine when the 3D LUT runs on an input
 * whose height is not a multiple of 8. iHD forces that alignment itself only on
 * its scaler path (Wa_16021308716), which this filter does not use, so the filter
 * never hands the LUT such a height. See the module README. */
#define LUT_HEIGHT_ALIGN 8

typedef struct HableLUTVAAPIContext {
    VAAPIVPPContext vpp_ctx; /* Must be first for the shared VPP lifecycle. */
    VASurfaceID lut_surface;
    /* P010 surfaces of the aligned height; NULL when the input height is already aligned. */
    AVBufferRef *stage_frames_ref;
    /* The copy into those surfaces has its own context. With the copy and the LUT
     * alternating in one context, iHD 26.1.5 left the LUT out of every picture after
     * the first; the cause inside the driver was not traced. */
    VAContextID stage_context;
} HableLUTVAAPIContext;

static void lut_pipeline_uninit(AVFilterContext *avctx)
{
    HableLUTVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp = &ctx->vpp_ctx;
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
    ctx->vpp_ctx.pipeline_uninit = lut_pipeline_uninit;
    ctx->vpp_ctx.build_filter_params = lut_build_params;
    ctx->vpp_ctx.output_format = AV_PIX_FMT_NV12;
    return 0;
}

static const AVOption tonemap_vaapi_lut_options[] = { { NULL } };
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
    .p.description = NULL_IF_CONFIG_SMALL("Fixed Hable HDR10 to SDR using a VAAPI 3D LUT"),
    .p.priv_class = &tonemap_vaapi_lut_class,
    .priv_size = sizeof(HableLUTVAAPIContext),
    .init = lut_init,
    .uninit = ff_vaapi_vpp_ctx_uninit,
    FILTER_INPUTS(lut_inputs),
    FILTER_OUTPUTS(lut_outputs),
    FILTER_QUERY_FUNC2(ff_vaapi_vpp_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};

/*
 * V4L2 mem2mem decoders
 *
 * Copyright (C) 2017 Alexis Ballier <aballier@gentoo.org>
 * Copyright (C) 2017 Jorge Ramirez <jorge.ramirez-ortiz@linaro.org>
 *
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

#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/pixfmt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/decode.h"

#include "libavcodec/hwaccel.h"
#include "libavcodec/internal.h"

#include "v4l2_context.h"
#include "v4l2_m2m.h"
#include "v4l2_fmt.h"

static void ff_v4l2_capture_set_crop(V4L2m2mContext *s, int width, int height)
{
    struct v4l2_selection selection;
    struct v4l2_crop crop;
    int ret;

    memset(&selection, 0, sizeof(struct v4l2_selection));
    selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selection.r.width = width;
    selection.r.height = height;
    ret = ioctl(s->fd, VIDIOC_S_SELECTION, &selection);
    if (!ret) {
        ret = ioctl(s->fd, VIDIOC_G_SELECTION, &selection);
        if (ret) {
            av_log(s->avctx, AV_LOG_WARNING, "VIDIOC_G_SELECTION ioctl\n");
        } else {
            av_log(s->avctx, AV_LOG_DEBUG, "crop output %dx%d\n", selection.r.width, selection.r.height);
            /* update the size of the resulting frame */
            s->capture.height = selection.r.height;
            s->capture.width  = selection.r.width;
        }
    } else {
        av_log(s->avctx, AV_LOG_WARNING, "VIDIOC_S_SELECTION ioctl\n");
        // use S_CROP and G_CROP
        struct v4l2_crop crop;
        memset(&crop, 0, sizeof(struct v4l2_crop));
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        crop.c.width = width;
        crop.c.height = height;
        ret = ioctl(s->fd, VIDIOC_S_CROP, &crop);
        if (!ret) {
            ret = ioctl(s->fd, VIDIOC_G_CROP, &crop);
            if (ret) {
                av_log(s->avctx, AV_LOG_WARNING, "VIDIOC_G_CROP ioctl\n");
            } else {
                av_log(s->avctx, AV_LOG_DEBUG, "crop output %dx%d\n", crop.c.width, crop.c.height);
                /* update the size of the resulting frame */
                s->capture.height = crop.c.height;
                s->capture.width  = crop.c.width;
            }
        } else {
            av_log(s->avctx, AV_LOG_ERROR, "VIDIOC_S_CROP ioctl\n");
        }
    }
}

static int gsc_try_start(AVCodecContext *avctx, V4L2Context *mfc_capture)
{
    V4L2m2mContext *mfc = ((V4L2m2mPriv*)avctx->priv_data)->context;
    V4L2m2mContext *gsc = ((V4L2m2mPriv*)avctx->priv_data)->convert;
    V4L2Context *const capture = &gsc->capture;
    V4L2Context *const output = &gsc->output;
    int ret;

    av_log(avctx, AV_LOG_DEBUG, "== start configuring GSC ==\n");

    /* 0. set GSC output settings from MFC capture */
    output->num_buffers = mfc_capture->num_buffers;
    memcpy(&output->format, &mfc_capture->format, sizeof(struct v4l2_format));
    output->format.type = output->type;
    output->av_pix_fmt = mfc_capture->av_pix_fmt;

    /* 1. probe device and set formats */
    ret = ff_v4l2_m2m_device_init(avctx, gsc);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "can't configure converter\n");
        return ret;
    }

    /* 2. init the output context with DMABUF buffers */
    if (!output->buffers) {
        ret = ff_v4l2_context_init_full(output, V4L2_MEMORY_DMABUF, mfc_capture);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "can't request output buffers\n");
            return AVERROR(ENOMEM);
        }
    }

    /* 3. get the capture format */
    capture->format.type = capture->type;
    ret = ioctl(gsc->fd, VIDIOC_G_FMT, &capture->format);
    if (ret) {
        av_log(avctx, AV_LOG_WARNING, "VIDIOC_G_FMT ioctl\n");
        return ret;
    }

    /* 3.1 update the AVCodecContext */
    avctx->pix_fmt = ff_v4l2_format_v4l2_to_avfmt(capture->format.fmt.pix_mp.pixelformat, AV_CODEC_ID_RAWVIDEO);
    capture->av_pix_fmt = avctx->pix_fmt;

    /* 4. set the crop parameters */
    ff_v4l2_capture_set_crop(gsc, avctx->coded_width, avctx->coded_height);


    /* 5. init the capture context now that we have the capture format */
    if (!capture->buffers) {
        ret = ff_v4l2_context_init(capture);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "can't request capture buffers\n");
            return AVERROR(ENOMEM);
        }
    }

    /* 6. start the output process */
    ret = ff_v4l2_context_set_status(output, VIDIOC_STREAMON);
    if (ret) {
        av_log(avctx, AV_LOG_DEBUG, "VIDIOC_STREAMON, on GSC output context\n");
        return ret;
    }

    /* 7. start the capture process */
    ret = ff_v4l2_context_set_status(capture, VIDIOC_STREAMON);
    if (ret) {
        av_log(avctx, AV_LOG_DEBUG, "VIDIOC_STREAMON, on GSC capture context\n");
        return ret;
    }

    av_log(avctx, AV_LOG_DEBUG, "== done configuring GSC ==\n");
    return 0;
}

static int v4l2_try_start(AVCodecContext *avctx)
{
    V4L2m2mContext *s = ((V4L2m2mPriv*)avctx->priv_data)->context;
    V4L2Context *const capture = &s->capture;
    V4L2Context *const output = &s->output;
    int ret;

    /* 1. start the output process */
    if (!output->streamon) {
        ret = ff_v4l2_context_set_status(output, VIDIOC_STREAMON);
        if (ret < 0) {
            av_log(avctx, AV_LOG_DEBUG, "VIDIOC_STREAMON on output context\n");
            return ret;
        }
    }

    if (capture->streamon)
        return 0;

    av_log(avctx, AV_LOG_DEBUG, "== start configuring MFC capture ==\n");

    /* 2. get the capture format */
    capture->format.type = capture->type;
    ret = ioctl(s->fd, VIDIOC_G_FMT, &capture->format);
    if (ret) {
        av_log(avctx, AV_LOG_WARNING, "VIDIOC_G_FMT ioctl\n");
        return ret;
    }

    /* 4. init the capture context now that we have the capture format */
    if (!capture->buffers) {
        ret = ff_v4l2_context_init(capture);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "can't request capture buffers\n");
            return AVERROR(ENOMEM);
        }
    }

    av_log(avctx, AV_LOG_DEBUG, "== done configuring MFC capture ==\n");

    /* check if conversion is needed */
    capture->av_pix_fmt = ff_v4l2_format_v4l2_to_avfmt(capture->format.fmt.pix_mp.pixelformat, AV_CODEC_ID_RAWVIDEO);
    if (avctx->pix_fmt != capture->av_pix_fmt) {
        av_log(avctx, AV_LOG_WARNING, "== decoder will use converter ==\n");
        s->output_drm = 0;
        s->output_convert = 1;
        /* 3. configure GSC for conversion */
        ret = gsc_try_start(avctx, capture);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "can't configure converter\n");
            return AVERROR_EXIT;
        }
    } else {
        /* 2.1 update the AVCodecContext */
        capture->av_pix_fmt = avctx->pix_fmt;
        /* 3. set the crop parameters */
        ff_v4l2_capture_set_crop(s, avctx->coded_width, avctx->coded_height);
    }

    /* 5. start the capture process */
    ret = ff_v4l2_context_set_status(capture, VIDIOC_STREAMON);
    if (ret) {
        av_log(avctx, AV_LOG_DEBUG, "VIDIOC_STREAMON, on capture context\n");
        return ret;
    }

    return 0;
}

static int v4l2_prepare_decoder(V4L2m2mContext *s)
{
    struct v4l2_event_subscription sub;
    V4L2Context *output = &s->output;
    int ret;

    /**
     * requirements
     */
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    ret = ioctl(s->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    if ( ret < 0) {
        if (output->height == 0 || output->width == 0) {
            av_log(s->avctx, AV_LOG_ERROR,
                "the v4l2 driver does not support VIDIOC_SUBSCRIBE_EVENT\n"
                "you must provide codec_height and codec_width on input\n");
            return ret;
        }
    }

    return 0;
}

static int v4l2_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    V4L2m2mContext *s = ((V4L2m2mPriv*)avctx->priv_data)->context;
    V4L2Context *const capture = &s->capture;
    V4L2Context *const output = &s->output;
    AVPacket avpkt = {0};
    int ret;

    ret = ff_decode_get_packet(avctx, &avpkt);
    if (ret < 0 && ret != AVERROR_EOF)
        return ret;

    if (s->draining)
        goto dequeue;

    ret = ff_v4l2_context_enqueue_packet(output, &avpkt);
    if (ret < 0) {
        if (ret != AVERROR(ENOMEM))
           return ret;
        /* no input buffers available, continue dequeing */
    }

    if (avpkt.size) {
        ret = v4l2_try_start(avctx);
        if (ret) {
            av_packet_unref(&avpkt);
            /* cant recover */
            if (ret == AVERROR(ENOMEM))
                return ret;

            return 0;
        }
    }

dequeue:
    av_packet_unref(&avpkt);
    return ff_v4l2_context_dequeue_frame(capture, frame);
}

static av_cold int v4l2_decode_init(AVCodecContext *avctx)
{
    V4L2Context *capture, *output;
    V4L2m2mContext *s;
    int ret;

    ret = ff_v4l2_m2m_create_context(avctx, &s);
    if (ret < 0)
        return ret;

    capture = &s->capture;
    output = &s->output;

    /* ff_v4l2_m2m_create_context for GSC converter */

    V4L2m2mContext *gsc;
    V4L2m2mPriv *priv = avctx->priv_data;

    gsc = av_mallocz(sizeof(V4L2m2mContext));
    if (!gsc)
        return AVERROR(ENOMEM);

    priv->convert_ref = av_buffer_create((uint8_t *) gsc, sizeof(V4L2m2mContext),
                                         &v4l2_m2m_destroy_context, NULL, 0);
    if (!priv->convert_ref) {
        av_freep(gsc);
        return AVERROR(ENOMEM);
    }

    priv->convert = gsc;

    priv->convert->capture.num_buffers = priv->num_capture_buffers;
    priv->convert->self_ref = priv->convert_ref;

    gsc->output.height = gsc->capture.height = avctx->coded_height;
    gsc->output.width = gsc->capture.width = avctx->coded_width;
    gsc->output.av_codec_id = AV_CODEC_ID_RAWVIDEO;
    gsc->output.av_pix_fmt = AV_PIX_FMT_NONE;
    gsc->capture.av_codec_id = AV_CODEC_ID_RAWVIDEO;
    gsc->capture.av_pix_fmt = avctx->pix_fmt;

    priv->convert->device_type = V4L2_DEVICE_TYPE_CONVERTER;
    priv->context->device_type = V4L2_DEVICE_TYPE_DECODER;

    s->output_convert = 0;
    gsc->output_convert = 0;

    /* -------------------------------------------- */

    /* if these dimensions are invalid (ie, 0 or too small) an event will be raised
     * by the v4l2 driver; this event will trigger a full pipeline reconfig and
     * the proper values will be retrieved from the kernel driver.
     */
    output->height = capture->height = avctx->coded_height;
    output->width = capture->width = avctx->coded_width;

    output->av_codec_id = avctx->codec_id;
    output->av_pix_fmt  = AV_PIX_FMT_NONE;

    capture->av_codec_id = AV_CODEC_ID_RAWVIDEO;
    capture->av_pix_fmt = avctx->pix_fmt;

    /* the client requests the codec to generate DRM frames:
     *   - data[0] will therefore point to the returned AVDRMFrameDescriptor
     *       check the ff_v4l2_buffer_to_avframe conversion function.
     *   - the DRM frame format is passed in the DRM frame descriptor layer.
     *       check the v4l2_get_drm_frame function.
     */
    if (ff_get_format(avctx, avctx->codec->pix_fmts) == AV_PIX_FMT_DRM_PRIME) {
        s->output_drm = 1;
        gsc->output_drm = 1;
    }

    ret = ff_v4l2_m2m_codec_init(avctx);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "can't configure decoder\n");
        return ret;
    }

    return v4l2_prepare_decoder(s);
}

#define OFFSET(x) offsetof(V4L2m2mPriv, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    V4L_M2M_DEFAULT_OPTS,
    { "num_capture_buffers", "Number of buffers in the capture context",
        OFFSET(num_capture_buffers), AV_OPT_TYPE_INT, {.i64 = 16}, 8, INT_MAX, FLAGS },
    { NULL},
};

static const AVCodecHWConfigInternal *v4l2_m2m_hw_configs[] = {
    HW_CONFIG_INTERNAL(DRM_PRIME),
    NULL
};

#define M2MDEC_CLASS(NAME) \
    static const AVClass v4l2_m2m_ ## NAME ## _dec_class = { \
        .class_name = #NAME "_v4l2_m2m_decoder", \
        .item_name  = av_default_item_name, \
        .option     = options, \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define M2MDEC(NAME, LONGNAME, CODEC, bsf_name) \
    M2MDEC_CLASS(NAME) \
    AVCodec ff_ ## NAME ## _v4l2m2m_decoder = { \
        .name           = #NAME "_v4l2m2m" , \
        .long_name      = NULL_IF_CONFIG_SMALL("V4L2 mem2mem " LONGNAME " decoder wrapper"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = CODEC , \
        .priv_data_size = sizeof(V4L2m2mPriv), \
        .priv_class     = &v4l2_m2m_ ## NAME ## _dec_class, \
        .init           = v4l2_decode_init, \
        .receive_frame  = v4l2_receive_frame, \
        .close          = ff_v4l2_m2m_codec_end, \
        .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_NONE}, \
        .bsfs           = bsf_name, \
        .hw_configs     = v4l2_m2m_hw_configs, \
        .capabilities   = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | \
                          AV_CODEC_CAP_AVOID_PROBING, \
        .wrapper_name   = "v4l2m2m", \
    };

M2MDEC(h264,  "H.264", AV_CODEC_ID_H264,       "h264_mp4toannexb");
M2MDEC(hevc,  "HEVC",  AV_CODEC_ID_HEVC,       "hevc_mp4toannexb");
M2MDEC(mpeg1, "MPEG1", AV_CODEC_ID_MPEG1VIDEO, NULL);
M2MDEC(mpeg2, "MPEG2", AV_CODEC_ID_MPEG2VIDEO, NULL);
M2MDEC(mpeg4, "MPEG4", AV_CODEC_ID_MPEG4,      NULL);
M2MDEC(h263,  "H.263", AV_CODEC_ID_H263,       NULL);
M2MDEC(vc1 ,  "VC1",   AV_CODEC_ID_VC1,        NULL);
M2MDEC(vp8,   "VP8",   AV_CODEC_ID_VP8,        NULL);
M2MDEC(vp9,   "VP9",   AV_CODEC_ID_VP9,        NULL);

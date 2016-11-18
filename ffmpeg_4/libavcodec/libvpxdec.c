/*
 * Copyright (c) 2010, Google, Inc.
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

/**
 * @file
 * VP8 decoder support via libvpx
 */

#define VPX_CODEC_DISABLE_COMPAT 1
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "avcodec.h"
#include "internal.h"
#include "libvpx.h"

#define BOOL int
#define TRUE 1
#define FALSE 0

typedef struct VP8DecoderContext {
    struct vpx_codec_ctx decoder;
	struct vpx_codec_ctx decoder_alpha;
} VP8Context;

static av_cold int vpx_init(AVCodecContext *avctx, const struct vpx_codec_iface *iface, BOOL bAlpha)
{
    VP8Context *ctx = avctx->priv_data;

    struct vpx_codec_dec_cfg deccfg = {
        /* token partitions+1 would be a decent choice */
        .threads = FFMIN(avctx->thread_count, 16)
    };

    av_log(avctx, AV_LOG_INFO, "FFMIN(avctx->thread_count, 16) %d\n", FFMIN(avctx->thread_count, 16));
    av_log(avctx, AV_LOG_INFO, "vpx_codec_version %s\n", vpx_codec_version_str());
    av_log(avctx, AV_LOG_VERBOSE, "%s\n", vpx_codec_build_config());

    if (vpx_codec_dec_init(&ctx->decoder, iface, &deccfg, 0) != VPX_CODEC_OK) {
        const char *error = vpx_codec_error(&ctx->decoder);
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize decoder: %s\n",
               error);
        return AVERROR(EINVAL);
    }

	if (bAlpha)	{
		av_log(avctx, AV_LOG_INFO, "Initializing alpha decoder\n");
		if (vpx_codec_dec_init(&ctx->decoder_alpha, iface, &deccfg, 0) != VPX_CODEC_OK) {
			const char *error = vpx_codec_error(&ctx->decoder_alpha);
			av_log(avctx, AV_LOG_ERROR, "Failed to initialize alpha decoder: %s\n",
				   error);
			return AVERROR(EINVAL);
		}
	}

	if (bAlpha)
		avctx->pix_fmt = AV_PIX_FMT_YUVA420P;
	else
		avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    return 0;
}

static int vp8_decode(AVCodecContext *avctx,
                      void *data, int *got_frame, AVPacket *avpkt)
{
    VP8Context *ctx = avctx->priv_data;
    AVFrame *picture = data;
    const void *iter = NULL;
    struct vpx_image *img;
    int ret;

    if (vpx_codec_decode(&ctx->decoder, avpkt->data, avpkt->size, NULL, 0) !=
        VPX_CODEC_OK) {
        const char *error  = vpx_codec_error(&ctx->decoder);
        const char *detail = vpx_codec_error_detail(&ctx->decoder);

        av_log(avctx, AV_LOG_ERROR, "Failed to decode frame: %s\n", error);
        if (detail)
            av_log(avctx, AV_LOG_ERROR, "  Additional information: %s\n",
                   detail);
        return AVERROR_INVALIDDATA;
    }

    if ((img = vpx_codec_get_frame(&ctx->decoder, &iter))) {
        if (img->fmt != VPX_IMG_FMT_I420) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported output colorspace (%d)\n",
                   img->fmt);
            return AVERROR_INVALIDDATA;
        }

        if ((int) img->d_w != avctx->width || (int) img->d_h != avctx->height) {
            av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n",
                   avctx->width, avctx->height, img->d_w, img->d_h);
            ret = ff_set_dimensions(avctx, img->d_w, img->d_h);
            if (ret < 0)
                return ret;
        }
        if ((ret = ff_get_buffer(avctx, picture, 0)) < 0)
            return ret;
        av_image_copy(picture->data, picture->linesize, (const uint8_t **)img->planes,
                      img->stride, avctx->pix_fmt, img->d_w, img->d_h);
        *got_frame           = 1;
    }
    return avpkt->size;
}

static int vp8alpha_decode(AVCodecContext *avctx,
                      void *data, int *got_frame, AVPacket *avpkt)
{
    if (avpkt->side_data_elems == 0)
    {
	int ret = vp8_decode(avctx, data, got_frame, avpkt);
	if (ret < 0) return ret;

	av_log(avctx, AV_LOG_INFO, "Alpha decoder : got YUV image no alpha\n");
	// copy A
	AVFrame * picture = data;
	uint8_t * pData = picture->data[3];
	for (int i = 0; i < avctx->height; i++)
	{
		memset(pData, 0xFF, FFABS(picture->linesize[3]));
		pData += picture->linesize[3];
	}
	return avpkt->size;
    }
  
    VP8Context * ctx = avctx->priv_data;
    AVFrame * picture = data;
    const void *iter = NULL;
    const void *iter_alpha = NULL;
    struct vpx_image *img;
    struct vpx_image *img_alpha;
    int ret;
    
	if (vpx_codec_decode(&ctx->decoder, avpkt->data, avpkt->size, NULL, 0) != VPX_CODEC_OK) {
        const char *error  = vpx_codec_error(&ctx->decoder);
        const char *detail = vpx_codec_error_detail(&ctx->decoder);

        av_log(avctx, AV_LOG_ERROR, "Failed to decode frame: %s\n", error);
        if (detail)
            av_log(avctx, AV_LOG_ERROR, "  Additional information: %s\n",
                   detail);
        return AVERROR_INVALIDDATA;
    }

	if ((img = vpx_codec_get_frame(&ctx->decoder, &iter))) {
        if (img->fmt != VPX_IMG_FMT_I420) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported output colorspace (%d)\n",
                   img->fmt);
            return AVERROR_INVALIDDATA;
        }

        if ((int) img->d_w != avctx->width || (int) img->d_h != avctx->height) {
            av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n",
                   avctx->width, avctx->height, img->d_w, img->d_h);
            ret = ff_set_dimensions(avctx, img->d_w, img->d_h);
            if (ret < 0)
                return ret;
        }
    }

	int side_data_size = 0;
	uint8_t * side_data = av_packet_get_side_data(avpkt, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &side_data_size);

	if (vpx_codec_decode(&ctx->decoder_alpha, side_data + 8, side_data_size - 8, NULL, 0) != VPX_CODEC_OK) {
        const char *error  = vpx_codec_error(&ctx->decoder_alpha);
        const char *detail = vpx_codec_error_detail(&ctx->decoder_alpha);

        av_log(avctx, AV_LOG_ERROR, "Failed to decode alpha frame: %s\n", error);
        if (detail)
            av_log(avctx, AV_LOG_ERROR, "  Additional information: %s\n",
                   detail);
        return AVERROR_INVALIDDATA;
    }

	if ((img_alpha = vpx_codec_get_frame(&ctx->decoder_alpha, &iter_alpha))) {
        if (img_alpha->fmt != VPX_IMG_FMT_I420) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported output alpha colorspace (%d)\n",
                   img_alpha->fmt);
            return AVERROR_INVALIDDATA;
        }

        if ((int) img_alpha->d_w != avctx->width || (int) img_alpha->d_h != avctx->height) {
            av_log(avctx, AV_LOG_INFO, "alpha dimension change! %dx%d -> %dx%d\n",
                   avctx->width, avctx->height, img_alpha->d_w, img_alpha->d_h);
            ret = ff_set_dimensions(avctx, img_alpha->d_w, img_alpha->d_h);
            if (ret < 0)
                return ret;
        }
    }

  //av_log(avctx, AV_LOG_INFO, "Alpha decoder : got A image\n");

	// copy YUV
    if ((ret = ff_get_buffer(avctx, picture, 0)) < 0)
        return ret;

	av_image_copy(picture->data, picture->linesize, (const uint8_t **)img->planes,
                    img->stride, avctx->pix_fmt, img->d_w, img->d_h);

	int bwidth = av_image_get_linesize(AV_PIX_FMT_YUVA420P, img->d_w, 3);
	av_log(avctx, AV_LOG_INFO, "Alpha decoder : bwidth  %d\n", bwidth);

	if (bwidth < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_image_get_linesize failed\n");
		return -1;
	}
	// copy A
	av_image_copy_plane(picture->data[3], picture->linesize[3],
                        img_alpha->planes[VPX_PLANE_Y], img->stride[VPX_PLANE_Y],
                        bwidth, img->d_h);

	*got_frame = 1;

	return avpkt->size;
}

static av_cold int vp8_free(AVCodecContext *avctx)
{
    VP8Context *ctx = avctx->priv_data;
    vpx_codec_destroy(&ctx->decoder);
    return 0;
}

static av_cold int vp8alpha_free(AVCodecContext *avctx)
{
    VP8Context *ctx = avctx->priv_data;
    vpx_codec_destroy(&ctx->decoder);
	vpx_codec_destroy(&ctx->decoder_alpha);
    return 0;
}

#if CONFIG_LIBVPX_VP8_DECODER
static av_cold int vp8_init(AVCodecContext *avctx)
{
    return vpx_init(avctx, &vpx_codec_vp8_dx_algo, FALSE);
}

static av_cold int vp8alpha_init(AVCodecContext *avctx)
{
    return vpx_init(avctx, &vpx_codec_vp8_dx_algo, TRUE);
}

AVCodec ff_libvpx_vp8_decoder = {
    .name           = "libvpx",
    .long_name      = NULL_IF_CONFIG_SMALL("libvpx VP8"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_VP8,
    .priv_data_size = sizeof(VP8Context),
    .init           = vp8_init,
    .close          = vp8_free,
    .decode         = vp8_decode,
    .capabilities   = CODEC_CAP_AUTO_THREADS | CODEC_CAP_DR1,
};

AVCodec ff_libvpx_vp8alpha_decoder = {
    .name           = "libvpxalpha",
    .long_name      = NULL_IF_CONFIG_SMALL("libvpx VP8 alpha"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_VP8,
    .priv_data_size = sizeof(VP8Context),
    .init           = vp8alpha_init,
    .close          = vp8alpha_free,
    .decode         = vp8alpha_decode,
    .capabilities   = CODEC_CAP_AUTO_THREADS | CODEC_CAP_DR1,
};

#endif /* CONFIG_LIBVPX_VP8_DECODER */

#if CONFIG_LIBVPX_VP9_DECODER
static av_cold int vp9_init(AVCodecContext *avctx)
{
    return vpx_init(avctx, &vpx_codec_vp9_dx_algo, FALSE);
}

AVCodec ff_libvpx_vp9_decoder = {
    .name           = "libvpx-vp9",
    .long_name      = NULL_IF_CONFIG_SMALL("libvpx VP9"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_VP9,
    .priv_data_size = sizeof(VP8Context),
    .init           = vp9_init,
    .close          = vp8_free,
    .decode         = vp8_decode,
    .capabilities   = CODEC_CAP_AUTO_THREADS | CODEC_CAP_DR1,
    .init_static_data = ff_vp9_init_static,
};
#endif /* CONFIG_LIBVPX_VP9_DECODER */

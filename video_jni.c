/**
 * 视频解码薄 JNI 包装 —— 直接调用 FFmpeg libavcodec / libswscale。
 *
 * 当前 Java 侧接口默认打开 H.264 解码器；FFmpeg 主构建同时包含 HEVC，
 * 方便后续扩展 VideoJni.decoderOpenForCodec(codecId, w, h)。
 *
 * 输出格式: RGBA packed (AV_PIX_FMT_RGBA) 或 packed YUV420P (Y + U + V)，可选择缩放。
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* ── 解码器句柄 ── */

typedef struct {
    AVCodecContext *codec_ctx;
    AVPacket       *packet;
    AVFrame        *decode_frame;
    AVFrame        *transfer_frame;
    AVFrame        *rgb_frame;
    AVFrame        *yuv_frame;
    AVBufferRef    *hw_device_ctx;
    struct SwsContext *rgba_sws_ctx;
    struct SwsContext *yuv_sws_ctx;
    enum AVPixelFormat hw_pix_fmt;
    enum AVHWDeviceType hw_device_type;
    int             rgba_sws_src_w, rgba_sws_src_h, rgba_sws_dst_w, rgba_sws_dst_h;
    int             yuv_sws_src_w, yuv_sws_src_h, yuv_sws_dst_w, yuv_sws_dst_h;
    enum AVPixelFormat yuv_sws_dst_format;
    int             target_width;
    int             target_height;
    int             original_width;
    int             original_height;
    int             use_hwaccel;
    int64_t         last_frame_pts_nanos;
    char            hwaccel_name[32];
} VideoDecoderHandle;

/* ── 辅助：抛 Java 异常 ── */

static void throwException(JNIEnv *env, const char *msg) {
    jclass cls = (*env)->FindClass(env, "java/lang/RuntimeException");
    if (cls) (*env)->ThrowNew(env, cls, msg);
}

JNIEXPORT jint JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_sendPacketWithPts(
        JNIEnv *env, jclass cls, jlong handle,
        jbyteArray data, jint offset, jint length, jlong pts_nanos);

static enum AVPixelFormat getHwFormat(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    VideoDecoderHandle *h = (VideoDecoderHandle *) ctx->opaque;
    if (!h) {
        return pix_fmts[0];
    }
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == h->hw_pix_fmt) {
            return *p;
        }
    }
    return pix_fmts[0];
}

static enum AVHWDeviceType requestedDeviceType(const char *name) {
    if (!name || !name[0] || strcmp(name, "auto") == 0) {
        return AV_HWDEVICE_TYPE_NONE;
    }
    return av_hwdevice_find_type_by_name(name);
}

static void setHwaccelFallback(VideoDecoderHandle *h, const char *reason) {
    if (!h) {
        return;
    }
    snprintf(h->hwaccel_name, sizeof(h->hwaccel_name), "cpu(%s)", reason ? reason : "hwaccel-failed");
}

static int tryEnableHwaccel(const AVCodec *codec, AVCodecContext *ctx, VideoDecoderHandle *h,
        enum AVHWDeviceType requested) {
    int config_count = 0;
    int requested_matches = 0;
    int device_ctx_matches = 0;
    int create_failures = 0;
    for (int i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        config_count++;
        if (requested == AV_HWDEVICE_TYPE_NONE || config->device_type == requested) {
            requested_matches++;
        }
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            continue;
        }
        if (requested != AV_HWDEVICE_TYPE_NONE && config->device_type != requested) {
            continue;
        }
        device_ctx_matches++;

        AVBufferRef *device = NULL;
        if (av_hwdevice_ctx_create(&device, config->device_type, NULL, NULL, 0) < 0) {
            create_failures++;
            continue;
        }

        h->hw_device_ctx = device;
        h->hw_pix_fmt = config->pix_fmt;
        h->hw_device_type = config->device_type;
        h->use_hwaccel = 1;
        const char *device_name = av_hwdevice_get_type_name(config->device_type);
        if (device_name) {
            snprintf(h->hwaccel_name, sizeof(h->hwaccel_name), "%s", device_name);
        }
        ctx->hw_device_ctx = av_buffer_ref(device);
        ctx->get_format = getHwFormat;
        ctx->opaque = h;
        return 1;
    }
    if (config_count == 0) {
        setHwaccelFallback(h, "no-codec-hw-config");
    } else if (requested_matches == 0) {
        setHwaccelFallback(h, "requested-device-not-built");
    } else if (device_ctx_matches == 0) {
        setHwaccelFallback(h, "no-hw-device-ctx-config");
    } else if (create_failures > 0) {
        setHwaccelFallback(h, "device-create-failed");
    } else {
        setHwaccelFallback(h, "hwaccel-unavailable");
    }
    return 0;
}

static jlong decoderOpenForCodec(JNIEnv *env, jint codec_id, jint target_width, jint target_height,
        const char *hwaccel_name) {
    enum AVCodecID av_codec_id;
    switch (codec_id) {
        case 12:
            av_codec_id = AV_CODEC_ID_HEVC;
            break;
        case 7:
        default:
            av_codec_id = AV_CODEC_ID_H264;
            break;
    }

    const AVCodec *codec = avcodec_find_decoder(av_codec_id);
    if (!codec) {
        throwException(env, av_codec_id == AV_CODEC_ID_HEVC
                ? "FFmpeg 未包含 HEVC 解码器"
                : "FFmpeg 未包含 H.264 解码器");
        return 0;
    }

    VideoDecoderHandle *h = (VideoDecoderHandle *) calloc(1, sizeof(VideoDecoderHandle));
    if (!h) {
        throwException(env, "分配 VideoDecoderHandle 失败");
        return 0;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        free(h);
        throwException(env, "分配 AVCodecContext 失败");
        return 0;
    }

    h->codec_ctx     = ctx;
    h->packet        = av_packet_alloc();
    h->decode_frame  = av_frame_alloc();
    h->transfer_frame = av_frame_alloc();
    h->rgb_frame     = av_frame_alloc();
    h->yuv_frame     = av_frame_alloc();
    h->target_width  = target_width;
    h->target_height = target_height;
    h->hw_pix_fmt = AV_PIX_FMT_NONE;
    h->hw_device_type = AV_HWDEVICE_TYPE_NONE;
    h->yuv_sws_dst_format = AV_PIX_FMT_NONE;
    h->last_frame_pts_nanos = AV_NOPTS_VALUE;
    snprintf(h->hwaccel_name, sizeof(h->hwaccel_name), "%s", "cpu");

    if (!h->packet || !h->decode_frame || !h->transfer_frame || !h->rgb_frame || !h->yuv_frame) {
        av_packet_free(&h->packet);
        av_frame_free(&h->decode_frame);
        av_frame_free(&h->transfer_frame);
        av_frame_free(&h->rgb_frame);
        av_frame_free(&h->yuv_frame);
        avcodec_free_context(&h->codec_ctx);
        free(h);
        throwException(env, "分配 packet/frame 失败");
        return 0;
    }

    enum AVHWDeviceType requested = requestedDeviceType(hwaccel_name);
    if (hwaccel_name && hwaccel_name[0]
            && strcmp(hwaccel_name, "none") != 0
            && strcmp(hwaccel_name, "off") != 0) {
        tryEnableHwaccel(codec, ctx, h, requested);
    }

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        if (h->hw_device_ctx) av_buffer_unref(&h->hw_device_ctx);
        av_packet_free(&h->packet);
        av_frame_free(&h->decode_frame);
        av_frame_free(&h->transfer_frame);
        av_frame_free(&h->rgb_frame);
        avcodec_free_context(&h->codec_ctx);
        free(h);
        throwException(env, "avcodec_open2 失败");
        return 0;
    }

    return (jlong)(size_t) h;
}

/* ── decoderOpen: 兼容当前 Java 侧 H.264-only 接口 ── */

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_decoderOpen(
        JNIEnv *env, jclass cls, jint target_width, jint target_height) {
    return decoderOpenForCodec(env, 7, target_width, target_height, "none");
}

/* ── decoderOpenForCodec: 预留给后续 Java 侧按 B站 codecid 选择 H.264/HEVC ── */

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_decoderOpenForCodec(
        JNIEnv *env, jclass cls, jint codec_id, jint target_width, jint target_height) {
    return decoderOpenForCodec(env, codec_id, target_width, target_height, "none");
}

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_decoderOpenForCodecWithHwaccel(
        JNIEnv *env, jclass cls, jint codec_id, jint target_width, jint target_height, jstring hwaccel) {
    const char *hw = hwaccel ? (*env)->GetStringUTFChars(env, hwaccel, NULL) : NULL;
    jlong handle = decoderOpenForCodec(env, codec_id, target_width, target_height, hw ? hw : "auto");
    if (hw) {
        (*env)->ReleaseStringUTFChars(env, hwaccel, hw);
    }
    return handle;
}

/* ── getVideoFrame ── */

static void rememberFramePts(VideoDecoderHandle *h, AVFrame *frame) {
    if (!h || !frame) {
        return;
    }
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    h->last_frame_pts_nanos = pts != AV_NOPTS_VALUE ? pts : AV_NOPTS_VALUE;
}

JNIEXPORT jbyteArray JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_getVideoFrame(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h || !h->codec_ctx) {
        throwException(env, "解码器句柄无效");
        return NULL;
    }

    int ret = avcodec_receive_frame(h->codec_ctx, h->decode_frame);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return NULL;
        }
        return NULL;
    }

    AVFrame *frame = h->decode_frame;
    rememberFramePts(h, h->decode_frame);
    if (h->use_hwaccel && h->decode_frame->format == h->hw_pix_fmt) {
        av_frame_unref(h->transfer_frame);
        if (av_hwframe_transfer_data(h->transfer_frame, h->decode_frame, 0) < 0) {
            av_frame_unref(h->decode_frame);
            return NULL;
        }
        frame = h->transfer_frame;
    }

    int src_w = frame->width;
    int src_h = frame->height;

    if (src_w <= 0 || src_h <= 0) {
        av_frame_unref(h->decode_frame);
        return NULL;
    }

    if (h->original_width != src_w || h->original_height != src_h) {
        h->original_width  = src_w;
        h->original_height = src_h;
    }

    int dst_w = h->target_width  > 0 ? h->target_width  : src_w;
    int dst_h = h->target_height > 0 ? h->target_height : src_h;

    if (!h->rgba_sws_ctx || h->rgba_sws_src_w != src_w || h->rgba_sws_src_h != src_h
            || h->rgba_sws_dst_w != dst_w || h->rgba_sws_dst_h != dst_h) {
        if (h->rgba_sws_ctx) {
            sws_freeContext(h->rgba_sws_ctx);
        }
        h->rgba_sws_ctx = sws_getContext(
            src_w, src_h, frame->format,
                dst_w, dst_h, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, NULL, NULL, NULL);
        h->rgba_sws_src_w = src_w;
        h->rgba_sws_src_h = src_h;
        h->rgba_sws_dst_w = dst_w;
        h->rgba_sws_dst_h = dst_h;

        if (!h->rgba_sws_ctx) {
            av_frame_unref(h->decode_frame);
            throwException(env, "sws_getContext 失败");
            return NULL;
        }
    }

    if (!h->rgb_frame->data[0] || h->rgb_frame->width != dst_w || h->rgb_frame->height != dst_h) {
        av_frame_unref(h->rgb_frame);
        h->rgb_frame->format = AV_PIX_FMT_RGBA;
        h->rgb_frame->width  = dst_w;
        h->rgb_frame->height = dst_h;
        if (av_frame_get_buffer(h->rgb_frame, 1) < 0) {
            av_frame_unref(h->decode_frame);
            throwException(env, "分配 RGB 缓冲区失败");
            return NULL;
        }
    }

    sws_scale(h->rgba_sws_ctx,
              (const uint8_t * const *) frame->data,
              frame->linesize,
              0, src_h,
              h->rgb_frame->data,
              h->rgb_frame->linesize);

    int rgba_size = dst_w * dst_h * 4;
    jbyteArray result = (*env)->NewByteArray(env, rgba_size);
    if (!result) {
        av_frame_unref(h->decode_frame);
        return NULL;
    }

    jbyte *dst = (*env)->GetByteArrayElements(env, result, NULL);
    if (!dst) {
        av_frame_unref(h->decode_frame);
        return NULL;
    }

    int src_stride = h->rgb_frame->linesize[0];
    int dst_stride = dst_w * 4;
    const uint8_t *src = h->rgb_frame->data[0];

    for (int y = 0; y < dst_h; y++) {
        memcpy(dst + y * dst_stride, src + y * src_stride, dst_stride);
    }

    (*env)->ReleaseByteArrayElements(env, result, dst, 0);

    if (frame == h->transfer_frame) {
        av_frame_unref(h->transfer_frame);
    }
    av_frame_unref(h->decode_frame);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_getVideoFrameInto(
        JNIEnv *env, jclass cls, jlong handle, jbyteArray output) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h || !h->codec_ctx || !output) {
        throwException(env, "解码器句柄或输出缓冲区无效");
        return -1;
    }

    int ret = avcodec_receive_frame(h->codec_ctx, h->decode_frame);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        return -1;
    }

    AVFrame *frame = h->decode_frame;
    rememberFramePts(h, h->decode_frame);
    if (h->use_hwaccel && h->decode_frame->format == h->hw_pix_fmt) {
        av_frame_unref(h->transfer_frame);
        if (av_hwframe_transfer_data(h->transfer_frame, h->decode_frame, 0) < 0) {
            av_frame_unref(h->decode_frame);
            return -1;
        }
        frame = h->transfer_frame;
    }

    int src_w = frame->width;
    int src_h = frame->height;

    if (src_w <= 0 || src_h <= 0) {
        av_frame_unref(h->decode_frame);
        return -1;
    }

    if (h->original_width != src_w || h->original_height != src_h) {
        h->original_width  = src_w;
        h->original_height = src_h;
    }

    int dst_w = h->target_width  > 0 ? h->target_width  : src_w;
    int dst_h = h->target_height > 0 ? h->target_height : src_h;
    int rgba_size = dst_w * dst_h * 4;

    if ((*env)->GetArrayLength(env, output) < rgba_size) {
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        av_frame_unref(h->decode_frame);
        throwException(env, "RGBA 输出缓冲区不足");
        return -1;
    }

    if (!h->rgba_sws_ctx || h->rgba_sws_src_w != src_w || h->rgba_sws_src_h != src_h
            || h->rgba_sws_dst_w != dst_w || h->rgba_sws_dst_h != dst_h) {
        if (h->rgba_sws_ctx) {
            sws_freeContext(h->rgba_sws_ctx);
        }
        h->rgba_sws_ctx = sws_getContext(
            src_w, src_h, frame->format,
                dst_w, dst_h, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, NULL, NULL, NULL);
        h->rgba_sws_src_w = src_w;
        h->rgba_sws_src_h = src_h;
        h->rgba_sws_dst_w = dst_w;
        h->rgba_sws_dst_h = dst_h;

        if (!h->rgba_sws_ctx) {
            if (frame == h->transfer_frame) {
                av_frame_unref(h->transfer_frame);
            }
            av_frame_unref(h->decode_frame);
            throwException(env, "sws_getContext 失败");
            return -1;
        }
    }

    if (!h->rgb_frame->data[0] || h->rgb_frame->width != dst_w || h->rgb_frame->height != dst_h) {
        av_frame_unref(h->rgb_frame);
        h->rgb_frame->format = AV_PIX_FMT_RGBA;
        h->rgb_frame->width  = dst_w;
        h->rgb_frame->height = dst_h;
        if (av_frame_get_buffer(h->rgb_frame, 1) < 0) {
            if (frame == h->transfer_frame) {
                av_frame_unref(h->transfer_frame);
            }
            av_frame_unref(h->decode_frame);
            throwException(env, "分配 RGB 缓冲区失败");
            return -1;
        }
    }

    sws_scale(h->rgba_sws_ctx,
              (const uint8_t * const *) frame->data,
              frame->linesize,
              0, src_h,
              h->rgb_frame->data,
              h->rgb_frame->linesize);

    jbyte *dst = (*env)->GetByteArrayElements(env, output, NULL);
    if (!dst) {
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        av_frame_unref(h->decode_frame);
        return -1;
    }

    int src_stride = h->rgb_frame->linesize[0];
    int dst_stride = dst_w * 4;
    const uint8_t *src = h->rgb_frame->data[0];

    for (int y = 0; y < dst_h; y++) {
        memcpy(dst + y * dst_stride, src + y * src_stride, dst_stride);
    }

    (*env)->ReleaseByteArrayElements(env, output, dst, 0);

    if (frame == h->transfer_frame) {
        av_frame_unref(h->transfer_frame);
    }
    av_frame_unref(h->decode_frame);
    return 1;
}

JNIEXPORT jbyteArray JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_getVideoFrameYuv420(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h || !h->codec_ctx) {
        throwException(env, "解码器句柄无效");
        return NULL;
    }

    int ret = avcodec_receive_frame(h->codec_ctx, h->decode_frame);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return NULL;
        }
        return NULL;
    }

    AVFrame *frame = h->decode_frame;
    rememberFramePts(h, h->decode_frame);
    if (h->use_hwaccel && h->decode_frame->format == h->hw_pix_fmt) {
        av_frame_unref(h->transfer_frame);
        if (av_hwframe_transfer_data(h->transfer_frame, h->decode_frame, 0) < 0) {
            av_frame_unref(h->decode_frame);
            return NULL;
        }
        frame = h->transfer_frame;
    }

    int src_w = frame->width;
    int src_h = frame->height;
    if (src_w <= 0 || src_h <= 0) {
        av_frame_unref(h->decode_frame);
        return NULL;
    }

    if (h->original_width != src_w || h->original_height != src_h) {
        h->original_width = src_w;
        h->original_height = src_h;
    }

    int dst_w = h->target_width > 0 ? h->target_width : src_w;
    int dst_h = h->target_height > 0 ? h->target_height : src_h;
    if ((dst_w & 1) != 0 || (dst_h & 1) != 0) {
        av_frame_unref(h->decode_frame);
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        throwException(env, "YUV420 输出需要偶数宽高");
        return NULL;
    }

    if (!h->yuv_sws_ctx || h->yuv_sws_src_w != src_w || h->yuv_sws_src_h != src_h
            || h->yuv_sws_dst_w != dst_w || h->yuv_sws_dst_h != dst_h
            || h->yuv_sws_dst_format != AV_PIX_FMT_YUV420P) {
        if (h->yuv_sws_ctx) {
            sws_freeContext(h->yuv_sws_ctx);
        }
        h->yuv_sws_ctx = sws_getContext(src_w, src_h, frame->format,
                dst_w, dst_h, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, NULL, NULL, NULL);
        h->yuv_sws_src_w = src_w;
        h->yuv_sws_src_h = src_h;
        h->yuv_sws_dst_w = dst_w;
        h->yuv_sws_dst_h = dst_h;
        h->yuv_sws_dst_format = AV_PIX_FMT_YUV420P;
        if (!h->yuv_sws_ctx) {
            av_frame_unref(h->decode_frame);
            if (frame == h->transfer_frame) {
                av_frame_unref(h->transfer_frame);
            }
            throwException(env, "sws_getContext(YUV420) 失败");
            return NULL;
        }
    }

    if (!h->yuv_frame->data[0] || h->yuv_frame->format != AV_PIX_FMT_YUV420P
            || h->yuv_frame->width != dst_w || h->yuv_frame->height != dst_h) {
        av_frame_unref(h->yuv_frame);
        h->yuv_frame->format = AV_PIX_FMT_YUV420P;
        h->yuv_frame->width = dst_w;
        h->yuv_frame->height = dst_h;
        if (av_frame_get_buffer(h->yuv_frame, 1) < 0) {
            av_frame_unref(h->decode_frame);
            if (frame == h->transfer_frame) {
                av_frame_unref(h->transfer_frame);
            }
            throwException(env, "分配 YUV420 缓冲区失败");
            return NULL;
        }
    }

    sws_scale(h->yuv_sws_ctx,
              (const uint8_t * const *) frame->data,
              frame->linesize,
              0, src_h,
              h->yuv_frame->data,
              h->yuv_frame->linesize);

    int y_size = dst_w * dst_h;
    int uv_w = dst_w / 2;
    int uv_h = dst_h / 2;
    int uv_size = uv_w * uv_h;
    int yuv_size = y_size + uv_size * 2;
    jbyteArray result = (*env)->NewByteArray(env, yuv_size);
    if (!result) {
        av_frame_unref(h->decode_frame);
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        return NULL;
    }

    jbyte *dst = (*env)->GetByteArrayElements(env, result, NULL);
    if (!dst) {
        av_frame_unref(h->decode_frame);
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        return NULL;
    }

    for (int y = 0; y < dst_h; y++) {
        memcpy(dst + y * dst_w, h->yuv_frame->data[0] + y * h->yuv_frame->linesize[0], dst_w);
    }
    jbyte *u_dst = dst + y_size;
    jbyte *v_dst = u_dst + uv_size;
    for (int y = 0; y < uv_h; y++) {
        memcpy(u_dst + y * uv_w, h->yuv_frame->data[1] + y * h->yuv_frame->linesize[1], uv_w);
        memcpy(v_dst + y * uv_w, h->yuv_frame->data[2] + y * h->yuv_frame->linesize[2], uv_w);
    }

    (*env)->ReleaseByteArrayElements(env, result, dst, 0);

    if (frame == h->transfer_frame) {
        av_frame_unref(h->transfer_frame);
    }
    av_frame_unref(h->decode_frame);
    return result;
}

JNIEXPORT jbyteArray JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_getVideoFrameNv12(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h || !h->codec_ctx) {
        throwException(env, "解码器句柄无效");
        return NULL;
    }

    int ret = avcodec_receive_frame(h->codec_ctx, h->decode_frame);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return NULL;
        }
        return NULL;
    }

    AVFrame *frame = h->decode_frame;
    rememberFramePts(h, h->decode_frame);
    if (h->use_hwaccel && h->decode_frame->format == h->hw_pix_fmt) {
        av_frame_unref(h->transfer_frame);
        if (av_hwframe_transfer_data(h->transfer_frame, h->decode_frame, 0) < 0) {
            av_frame_unref(h->decode_frame);
            return NULL;
        }
        frame = h->transfer_frame;
    }

    int src_w = frame->width;
    int src_h = frame->height;
    if (src_w <= 0 || src_h <= 0) {
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        av_frame_unref(h->decode_frame);
        return NULL;
    }

    if (h->original_width != src_w || h->original_height != src_h) {
        h->original_width = src_w;
        h->original_height = src_h;
    }

    int dst_w = h->target_width > 0 ? h->target_width : src_w;
    int dst_h = h->target_height > 0 ? h->target_height : src_h;
    if ((dst_w & 1) != 0 || (dst_h & 1) != 0) {
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        av_frame_unref(h->decode_frame);
        throwException(env, "NV12 输出需要偶数宽高");
        return NULL;
    }

    if (!h->yuv_sws_ctx || h->yuv_sws_src_w != src_w || h->yuv_sws_src_h != src_h
            || h->yuv_sws_dst_w != dst_w || h->yuv_sws_dst_h != dst_h
            || h->yuv_sws_dst_format != AV_PIX_FMT_NV12) {
        if (h->yuv_sws_ctx) {
            sws_freeContext(h->yuv_sws_ctx);
        }
        h->yuv_sws_ctx = sws_getContext(src_w, src_h, frame->format,
                dst_w, dst_h, AV_PIX_FMT_NV12,
                SWS_BILINEAR, NULL, NULL, NULL);
        h->yuv_sws_src_w = src_w;
        h->yuv_sws_src_h = src_h;
        h->yuv_sws_dst_w = dst_w;
        h->yuv_sws_dst_h = dst_h;
        h->yuv_sws_dst_format = AV_PIX_FMT_NV12;
        if (!h->yuv_sws_ctx) {
            if (frame == h->transfer_frame) {
                av_frame_unref(h->transfer_frame);
            }
            av_frame_unref(h->decode_frame);
            throwException(env, "sws_getContext(NV12) 失败");
            return NULL;
        }
    }

    if (!h->yuv_frame->data[0] || h->yuv_frame->format != AV_PIX_FMT_NV12
            || h->yuv_frame->width != dst_w || h->yuv_frame->height != dst_h) {
        av_frame_unref(h->yuv_frame);
        h->yuv_frame->format = AV_PIX_FMT_NV12;
        h->yuv_frame->width = dst_w;
        h->yuv_frame->height = dst_h;
        if (av_frame_get_buffer(h->yuv_frame, 1) < 0) {
            if (frame == h->transfer_frame) {
                av_frame_unref(h->transfer_frame);
            }
            av_frame_unref(h->decode_frame);
            throwException(env, "分配 NV12 缓冲区失败");
            return NULL;
        }
    }

    sws_scale(h->yuv_sws_ctx,
              (const uint8_t * const *) frame->data,
              frame->linesize,
              0, src_h,
              h->yuv_frame->data,
              h->yuv_frame->linesize);

    int y_size = dst_w * dst_h;
    int uv_h = dst_h / 2;
    int uv_stride = dst_w;
    int nv12_size = y_size + uv_stride * uv_h;
    jbyteArray result = (*env)->NewByteArray(env, nv12_size);
    if (!result) {
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        av_frame_unref(h->decode_frame);
        return NULL;
    }

    jbyte *dst = (*env)->GetByteArrayElements(env, result, NULL);
    if (!dst) {
        if (frame == h->transfer_frame) {
            av_frame_unref(h->transfer_frame);
        }
        av_frame_unref(h->decode_frame);
        return NULL;
    }

    for (int y = 0; y < dst_h; y++) {
        memcpy(dst + y * dst_w, h->yuv_frame->data[0] + y * h->yuv_frame->linesize[0], dst_w);
    }
    jbyte *uv_dst = dst + y_size;
    for (int y = 0; y < uv_h; y++) {
        memcpy(uv_dst + y * uv_stride, h->yuv_frame->data[1] + y * h->yuv_frame->linesize[1], uv_stride);
    }

    (*env)->ReleaseByteArrayElements(env, result, dst, 0);

    if (frame == h->transfer_frame) {
        av_frame_unref(h->transfer_frame);
    }
    av_frame_unref(h->decode_frame);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_receiveFrameNoCopy(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h || !h->codec_ctx) {
        throwException(env, "解码器句柄无效");
        return -1;
    }

    int ret = avcodec_receive_frame(h->codec_ctx, h->decode_frame);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        return -1;
    }

    if (h->original_width != h->decode_frame->width || h->original_height != h->decode_frame->height) {
        h->original_width  = h->decode_frame->width;
        h->original_height = h->decode_frame->height;
    }

    rememberFramePts(h, h->decode_frame);

    av_frame_unref(h->decode_frame);
    return 1;
}

/* ── sendPacket ── */

JNIEXPORT jint JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_sendPacket(
        JNIEnv *env, jclass cls, jlong handle,
        jbyteArray data, jint offset, jint length) {

    return Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_sendPacketWithPts(
        env, cls, handle, data, offset, length, (jlong) AV_NOPTS_VALUE);
}

JNIEXPORT jint JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_sendPacketWithPts(
    JNIEnv *env, jclass cls, jlong handle,
    jbyteArray data, jint offset, jint length, jlong pts_nanos) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h || !h->codec_ctx) {
        throwException(env, "解码器句柄无效");
        return -1;
    }

    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) {
        return -1;
    }

    av_packet_unref(h->packet);
    int ret = av_new_packet(h->packet, length);
    if (ret < 0) {
        (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
        return -1;
    }
    memcpy(h->packet->data, bytes + offset, length);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);

    if (pts_nanos >= 0) {
        h->packet->pts = (int64_t) pts_nanos;
        h->packet->dts = AV_NOPTS_VALUE;
        h->packet->time_base = (AVRational) { 1, 1000000000 };
        h->codec_ctx->pkt_timebase = (AVRational) { 1, 1000000000 };
    } else {
        h->packet->pts = AV_NOPTS_VALUE;
        h->packet->dts = AV_NOPTS_VALUE;
    }

    ret = avcodec_send_packet(h->codec_ctx, h->packet);
    if (ret < 0) {
        return -1;
    }

    return 0;
}

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_getLastFramePtsNanos(
        JNIEnv *env, jclass cls, jlong handle) {
    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h) {
        return -1;
    }
    return h->last_frame_pts_nanos != AV_NOPTS_VALUE ? (jlong) h->last_frame_pts_nanos : -1;
}

/* ── flush ── */

JNIEXPORT void JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_flush(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (h && h->codec_ctx) {
        avcodec_flush_buffers(h->codec_ctx);
        h->last_frame_pts_nanos = AV_NOPTS_VALUE;
    }
}

JNIEXPORT jstring JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_getHwaccelName(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h || !h->hwaccel_name[0]) {
        return (*env)->NewStringUTF(env, "cpu");
    }
    return (*env)->NewStringUTF(env, h->hwaccel_name);
}

/* ── close ── */

JNIEXPORT void JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_close(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h) return;

    if (h->rgba_sws_ctx) sws_freeContext(h->rgba_sws_ctx);
    if (h->yuv_sws_ctx)  sws_freeContext(h->yuv_sws_ctx);
    if (h->rgb_frame)    av_frame_free(&h->rgb_frame);
    if (h->yuv_frame)    av_frame_free(&h->yuv_frame);
    if (h->transfer_frame) av_frame_free(&h->transfer_frame);
    if (h->decode_frame) av_frame_free(&h->decode_frame);
    if (h->packet)       av_packet_free(&h->packet);
    if (h->hw_device_ctx) av_buffer_unref(&h->hw_device_ctx);
    if (h->codec_ctx)    avcodec_free_context(&h->codec_ctx);
    free(h);
}

/* ── getDimensions: 返回原始宽高 (width << 32 | height) ── */

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_VideoJni_getDimensions(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h) return 0;

    return ((jlong) h->original_width << 32) | (jlong) (h->original_height & 0xFFFFFFFFL);
}

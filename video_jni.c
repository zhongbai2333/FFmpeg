/**
 * 视频解码薄 JNI 包装 —— 直接调用 FFmpeg libavcodec / libswscale。
 *
 * 当前 Java 侧接口默认打开 H.264 解码器；FFmpeg 主构建同时包含 HEVC，
 * 方便后续扩展 VideoJni.decoderOpenForCodec(codecId, w, h)。
 *
 * 输出格式: RGBA packed (AV_PIX_FMT_RGBA), 可选择缩放。
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* ── 解码器句柄 ── */

typedef struct {
    AVCodecContext *codec_ctx;
    AVPacket       *packet;
    AVFrame        *decode_frame;
    AVFrame        *rgb_frame;
    struct SwsContext *sws_ctx;
    int             sws_src_w, sws_src_h, sws_dst_w, sws_dst_h;
    int             target_width;
    int             target_height;
    int             original_width;
    int             original_height;
} VideoDecoderHandle;

/* ── 辅助：抛 Java 异常 ── */

static void throwException(JNIEnv *env, const char *msg) {
    jclass cls = (*env)->FindClass(env, "java/lang/RuntimeException");
    if (cls) (*env)->ThrowNew(env, cls, msg);
}

static jlong decoderOpenForCodec(JNIEnv *env, jint codec_id, jint target_width, jint target_height) {
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

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        throwException(env, "分配 AVCodecContext 失败");
        return 0;
    }

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        throwException(env, "avcodec_open2 失败");
        return 0;
    }

    VideoDecoderHandle *h = (VideoDecoderHandle *) calloc(1, sizeof(VideoDecoderHandle));
    if (!h) {
        avcodec_free_context(&ctx);
        throwException(env, "分配 VideoDecoderHandle 失败");
        return 0;
    }

    h->codec_ctx     = ctx;
    h->packet        = av_packet_alloc();
    h->decode_frame  = av_frame_alloc();
    h->rgb_frame     = av_frame_alloc();
    h->target_width  = target_width;
    h->target_height = target_height;

    if (!h->packet || !h->decode_frame || !h->rgb_frame) {
        av_packet_free(&h->packet);
        av_frame_free(&h->decode_frame);
        av_frame_free(&h->rgb_frame);
        avcodec_free_context(&h->codec_ctx);
        free(h);
        throwException(env, "分配 packet/frame 失败");
        return 0;
    }

    return (jlong)(size_t) h;
}

/* ── decoderOpen: 兼容当前 Java 侧 H.264-only 接口 ── */

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_bili_codec_VideoJni_decoderOpen(
        JNIEnv *env, jclass cls, jint target_width, jint target_height) {
    return decoderOpenForCodec(env, 7, target_width, target_height);
}

/* ── decoderOpenForCodec: 预留给后续 Java 侧按 B站 codecid 选择 H.264/HEVC ── */

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_bili_codec_VideoJni_decoderOpenForCodec(
        JNIEnv *env, jclass cls, jint codec_id, jint target_width, jint target_height) {
    return decoderOpenForCodec(env, codec_id, target_width, target_height);
}

/* ── getVideoFrame ── */

JNIEXPORT jbyteArray JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_bili_codec_VideoJni_getVideoFrame(
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

    int src_w = h->decode_frame->width;
    int src_h = h->decode_frame->height;

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

    if (!h->sws_ctx || h->sws_src_w != src_w || h->sws_src_h != src_h
            || h->sws_dst_w != dst_w || h->sws_dst_h != dst_h) {
        if (h->sws_ctx) {
            sws_freeContext(h->sws_ctx);
        }
        h->sws_ctx = sws_getContext(
                src_w, src_h, h->decode_frame->format,
                dst_w, dst_h, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, NULL, NULL, NULL);
        h->sws_src_w = src_w;
        h->sws_src_h = src_h;
        h->sws_dst_w = dst_w;
        h->sws_dst_h = dst_h;

        if (!h->sws_ctx) {
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

    sws_scale(h->sws_ctx,
              (const uint8_t * const *) h->decode_frame->data,
              h->decode_frame->linesize,
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

    av_frame_unref(h->decode_frame);
    return result;
}

/* ── sendPacket ── */

JNIEXPORT jint JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_bili_codec_VideoJni_sendPacket(
        JNIEnv *env, jclass cls, jlong handle,
        jbyteArray data, jint offset, jint length) {

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

    ret = avcodec_send_packet(h->codec_ctx, h->packet);
    if (ret < 0) {
        return -1;
    }

    return 0;
}

/* ── flush ── */

JNIEXPORT void JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_bili_codec_VideoJni_flush(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (h && h->codec_ctx) {
        avcodec_flush_buffers(h->codec_ctx);
    }
}

/* ── close ── */

JNIEXPORT void JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_bili_codec_VideoJni_close(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h) return;

    if (h->sws_ctx)      sws_freeContext(h->sws_ctx);
    if (h->rgb_frame)    av_frame_free(&h->rgb_frame);
    if (h->decode_frame) av_frame_free(&h->decode_frame);
    if (h->packet)       av_packet_free(&h->packet);
    if (h->codec_ctx)    avcodec_free_context(&h->codec_ctx);
    free(h);
}

/* ── getDimensions: 返回原始宽高 (width << 32 | height) ── */

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_bili_codec_VideoJni_getDimensions(
        JNIEnv *env, jclass cls, jlong handle) {

    VideoDecoderHandle *h = (VideoDecoderHandle *)(size_t) handle;
    if (!h) return 0;

    return ((jlong) h->original_width << 32) | (jlong) (h->original_height & 0xFFFFFFFFL);
}

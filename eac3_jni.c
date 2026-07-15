/**
 * E-AC-3 薄 JNI 包装 —— 直接调用 FFmpeg libavcodec / libavutil。
 *
 * 编译示例 (Windows x64, MSYS2 UCRT64):
 *   gcc -shared -o eac3_jni.dll eac3_jni.c \
 *       -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/win32" \
 *       -I install/include -L install/bin -lavcodec -lavutil \
 *       -Wl,--out-implib,libeac3_jni.dll.a -static-libgcc
 *
 * 编译示例 (Linux):
 *   gcc -shared -o libeac3_jni.so eac3_jni.c \
 *       -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
 *       -I install/include -L install/lib -lavcodec -lavutil
 *
 * 编译示例 (macOS):
 *   gcc -shared -o libeac3_jni.dylib eac3_jni.c \
 *       -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/darwin" \
 *       -I install/include -L install/lib -lavcodec -lavutil
 *
 * 原生 (Windows ARM64 via clang MSVC target):
 *   clang --target=aarch64-pc-windows-msvc -shared -Wno-microsoft-include \
 *       -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/win32" \
 *       -I install/include -L install/bin -lavcodec -lavutil \
 *       -o eac3_jni.dll eac3_jni.c
 *   Windows ARM64 自带完整 SDK，CRT 头文件齐全，无交叉编译问题。
 */

#include <jni.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>

/* ── 解码器句柄 ── */

typedef struct
{
    AVCodecContext *ctx;
    AVPacket *packet;
    AVFrame *frame;
} DecoderHandle;

/* ── 辅助：抛 Java 异常 ── */

static void throwException(JNIEnv *env, const char *msg)
{
    jclass cls = (*env)->FindClass(env, "java/lang/RuntimeException");
    if (cls)
        (*env)->ThrowNew(env, cls, msg);
}

static int validateByteArrayRange(JNIEnv *env, jbyteArray data, jint offset, jint length)
{
    if (!data)
    {
        throwException(env, "输入字节数组不能为空");
        return 0;
    }
    if (offset < 0 || length < 0)
    {
        throwException(env, "输入 offset/length 不能为负数");
        return 0;
    }
    jsize array_length = (*env)->GetArrayLength(env, data);
    if ((int64_t)offset + (int64_t)length > (int64_t)array_length)
    {
        throwException(env, "输入 offset/length 超出字节数组范围");
        return 0;
    }
    if (length > INT_MAX)
    {
        throwException(env, "输入 packet 过大");
        return 0;
    }
    return 1;
}

/* ── decoderOpen ── */

JNIEXPORT jlong JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_Eac3Jni_decoderOpen(
    JNIEnv *env, jclass cls)
{

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_EAC3);
    if (!codec)
    {
        throwException(env, "FFmpeg 未包含 E-AC-3 解码器");
        return 0;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        throwException(env, "分配 AVCodecContext 失败");
        return 0;
    }

    if (avcodec_open2(ctx, codec, NULL) < 0)
    {
        avcodec_free_context(&ctx);
        throwException(env, "avcodec_open2 失败");
        return 0;
    }

    DecoderHandle *h = (DecoderHandle *)calloc(1, sizeof(DecoderHandle));
    if (!h)
    {
        avcodec_free_context(&ctx);
        throwException(env, "分配 DecoderHandle 失败");
        return 0;
    }

    h->ctx = ctx;
    h->packet = av_packet_alloc();
    h->frame = av_frame_alloc();

    if (!h->packet || !h->frame)
    {
        av_packet_free(&h->packet);
        av_frame_free(&h->frame);
        avcodec_free_context(&h->ctx);
        free(h);
        throwException(env, "分配 packet/frame 失败");
        return 0;
    }

    return (jlong)(size_t)h;
}

/* ── decode ── */

JNIEXPORT jobjectArray JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_Eac3Jni_decode(
    JNIEnv *env, jclass cls, jlong handle,
    jbyteArray data, jint offset, jint length)
{

    DecoderHandle *h = (DecoderHandle *)(size_t)handle;
    if (!h || !h->ctx)
    {
        throwException(env, "解码器句柄无效");
        return NULL;
    }

    if (!validateByteArrayRange(env, data, offset, length))
    {
        return NULL;
    }

    /* 将 Java 字节数组拷贝到 av_malloc 分配的 packet buffer */
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes)
    {
        throwException(env, "GetByteArrayElements 失败");
        return NULL;
    }

    av_packet_unref(h->packet);
    int ret = av_new_packet(h->packet, length);
    if (ret < 0)
    {
        (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
        return NULL;
    }
    memcpy(h->packet->data, bytes + offset, length);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);

    /* 发往解码器 */
    ret = avcodec_send_packet(h->ctx, h->packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
    {
        return NULL;
    }

    /* 接收解码帧 */
    ret = avcodec_receive_frame(h->ctx, h->frame);
    if (ret < 0)
    {
        return NULL;
    }

    jobjectArray result = NULL;
    int samples = h->frame->nb_samples;
    int channels = h->frame->ch_layout.nb_channels;

    if (samples <= 0 || channels <= 0 || h->frame->format != AV_SAMPLE_FMT_FLTP || !h->frame->extended_data)
    {
        goto cleanup;
    }
    for (int ch = 0; ch < channels; ch++)
    {
        if (!h->frame->extended_data[ch])
        {
            goto cleanup;
        }
    }

    /* 构建 float[][] 返回: 外层 = channels, 内层 = samples */
    jclass floatArrayClass = (*env)->FindClass(env, "[F");
    if (!floatArrayClass)
    {
        goto cleanup;
    }

    result = (*env)->NewObjectArray(env, channels, floatArrayClass, NULL);
    if (!result)
    {
        goto cleanup;
    }

    for (int ch = 0; ch < channels; ch++)
    {
        jfloatArray channelArray = (*env)->NewFloatArray(env, samples);
        if (!channelArray)
        {
            result = NULL;
            goto cleanup;
        }

        const float *src = (const float *)h->frame->extended_data[ch];
        (*env)->SetFloatArrayRegion(env, channelArray, 0, samples, src);
        if ((*env)->ExceptionCheck(env))
        {
            (*env)->DeleteLocalRef(env, channelArray);
            result = NULL;
            goto cleanup;
        }

        (*env)->SetObjectArrayElement(env, result, ch, channelArray);
        (*env)->DeleteLocalRef(env, channelArray);
        if ((*env)->ExceptionCheck(env))
        {
            result = NULL;
            goto cleanup;
        }
    }

cleanup:
    av_frame_unref(h->frame);
    return result;
}

/* ── flush ── */

JNIEXPORT void JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_Eac3Jni_flush(
    JNIEnv *env, jclass cls, jlong handle)
{

    DecoderHandle *h = (DecoderHandle *)(size_t)handle;
    if (h && h->ctx)
    {
        avcodec_flush_buffers(h->ctx);
        av_packet_unref(h->packet);
        av_frame_unref(h->frame);
    }
}

/* ── close ── */

JNIEXPORT void JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_Eac3Jni_close(
    JNIEnv *env, jclass cls, jlong handle)
{

    DecoderHandle *h = (DecoderHandle *)(size_t)handle;
    if (!h)
        return;

    if (h->frame)
        av_frame_free(&h->frame);
    if (h->packet)
        av_packet_free(&h->packet);
    if (h->ctx)
        avcodec_free_context(&h->ctx);

    free(h);
}

/* ── version ── */

JNIEXPORT jstring JNICALL
Java_com_zhongbai233_net_1music_1can_1play_1bili_media_codec_Eac3Jni_version(
    JNIEnv *env, jclass cls)
{

    const char *ver = av_version_info();
    return (*env)->NewStringUTF(env, ver ? ver : "unknown");
}

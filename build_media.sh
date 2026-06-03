#!/bin/bash
# 构建最小 FFmpeg + JNI: E-AC-3 音频 + H.264/HEVC 视频
# 需要: gcc/clang, make, nasm, upx (可选), JDK
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="$SCRIPT_DIR/ffmpeg_install"

echo "=== FFmpeg minimal media build (E-AC-3 + H.264 + HEVC) ==="

HWACCEL_CONFIG=()
case "$OSTYPE" in
    darwin*)
        HWACCEL_CONFIG+=(
            --enable-hwaccel=h264_videotoolbox
            --enable-hwaccel=hevc_videotoolbox
        )
        ;;
    linux*)
        HWACCEL_CONFIG+=(
            --enable-vaapi
            --enable-hwaccel=h264_vaapi
            --enable-hwaccel=hevc_vaapi
        )
        ;;
    msys*|cygwin*|win32)
        HWACCEL_CONFIG+=(
            --enable-hwaccel=h264_d3d11va
            --enable-hwaccel=hevc_d3d11va
            --enable-hwaccel=h264_dxva2
            --enable-hwaccel=hevc_dxva2
        )
        ;;
esac

# ── 1. 构建 FFmpeg ──
./configure \
    --disable-everything \
    --enable-decoder=eac3 \
    --enable-decoder=h264 \
    --enable-decoder=hevc \
    --enable-parser=ac3 \
    --enable-parser=h264 \
    --enable-parser=hevc \
    --enable-bsf=h264_mp4toannexb \
    --enable-bsf=hevc_mp4toannexb \
    --enable-swscale \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avformat \
    --disable-avfilter \
    --disable-network \
    --disable-encoders \
    --disable-muxers \
    --disable-protocols \
    --disable-filters \
    --enable-small \
    --disable-debug \
    --enable-shared \
    --enable-w32threads \
    --disable-pthreads \
    --extra-ldflags="-static-libgcc -static-libstdc++" \
    --pkg-config-flags="--static" \
    --prefix="$INSTALL_DIR" \
    "${HWACCEL_CONFIG[@]}"

make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
make install

# ── 2. 构建 eac3_jni (音频) ──
echo "=== build eac3_jni ==="
JAVA_HOME="${JAVA_HOME:-}"
if [ -z "$JAVA_HOME" ]; then
    if command -v java &>/dev/null; then
        JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v java)")")")"
    fi
fi

JNI_INCLUDES="-I${JAVA_HOME}/include"
case "$OSTYPE" in
    darwin*)  JNI_INCLUDES="$JNI_INCLUDES -I${JAVA_HOME}/include/darwin" ;;
    linux*)   JNI_INCLUDES="$JNI_INCLUDES -I${JAVA_HOME}/include/linux" ;;
    msys*|cygwin*|win32) JNI_INCLUDES="$JNI_INCLUDES -I${JAVA_HOME}/include/win32" ;;
esac

LIBS="-L${INSTALL_DIR}/lib -L${INSTALL_DIR}/bin -lavcodec -lavutil"
EXTRA_FLAGS="-static-libgcc"

case "$OSTYPE" in
    darwin*)
        gcc -shared -o "$INSTALL_DIR/bin/libeac3_jni.dylib" "$SCRIPT_DIR/eac3_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS $EXTRA_FLAGS
        ;;
    linux*)
        gcc -shared -o "$INSTALL_DIR/lib/libeac3_jni.so" "$SCRIPT_DIR/eac3_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS $EXTRA_FLAGS
        ;;
    msys*|cygwin*|win32)
        gcc -shared -o "$INSTALL_DIR/bin/eac3_jni.dll" "$SCRIPT_DIR/eac3_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS \
            -Wl,--out-implib,libeac3_jni.dll.a $EXTRA_FLAGS
        ;;
esac

# ── 3. 构建 video_jni (视频) ──
echo "=== build video_jni ==="
LIBS_VIDEO="$LIBS -lswscale"

case "$OSTYPE" in
    darwin*)
        gcc -shared -o "$INSTALL_DIR/bin/libvideo_jni.dylib" "$SCRIPT_DIR/video_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS_VIDEO $EXTRA_FLAGS
        ;;
    linux*)
        gcc -shared -o "$INSTALL_DIR/lib/libvideo_jni.so" "$SCRIPT_DIR/video_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS_VIDEO $EXTRA_FLAGS
        ;;
    msys*|cygwin*|win32)
        gcc -shared -o "$INSTALL_DIR/bin/video_jni.dll" "$SCRIPT_DIR/video_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS_VIDEO \
            -Wl,--out-implib,libvideo_jni.dll.a $EXTRA_FLAGS
        ;;
esac

# ── 4. 报告 ──
echo ""
echo "=== Build complete ==="
case "$OSTYPE" in
    darwin*)
        LIB_DIR="$INSTALL_DIR/bin"
        LIBS_LIST=("libavutil" "libswresample" "libswscale" "libavcodec" "libeac3_jni" "libvideo_jni")
        EXT=".dylib"
        ;;
    linux*)
        LIB_DIR="$INSTALL_DIR/lib"
        LIBS_LIST=("libavutil" "libswresample" "libswscale" "libavcodec" "libeac3_jni" "libvideo_jni")
        EXT=".so"
        ;;
    msys*|cygwin*|win32)
        LIB_DIR="$INSTALL_DIR/bin"
        LIBS_LIST=("avutil-60" "swresample-6" "swscale-9" "avcodec-62" "eac3_jni" "video_jni")
        EXT=".dll"
        ;;
esac

for lib in "${LIBS_LIST[@]}"; do
    path="$LIB_DIR/${lib}${EXT}"
    if [ -f "$path" ]; then
        size=$(stat -c%s "$path" 2>/dev/null || stat -f%z "$path" 2>/dev/null)
        printf "  %-40s %6d bytes\n" "$(basename "$path")" "$size"
    else
        printf "  %-40s MISSING\n" "$(basename "$path")"
    fi
done

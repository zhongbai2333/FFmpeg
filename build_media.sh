#!/bin/bash
# 构建最小 FFmpeg + JNI: E-AC-3 音频 + H.264/HEVC 视频
# 需要: gcc/clang, make, nasm, upx (可选), JDK
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="$SCRIPT_DIR/ffmpeg_install"

echo "=== FFmpeg minimal media build (E-AC-3 + H.264 + HEVC) ==="

HWACCEL_CONFIG=()
ENABLE_LINUX_VAAPI="${ENABLE_LINUX_VAAPI:-1}"
case "$OSTYPE" in
    darwin*)
        HWACCEL_CONFIG+=(
            --enable-hwaccel=h264_videotoolbox
            --enable-hwaccel=hevc_videotoolbox
        )
        ;;
    linux*)
        # Linux bundle enables VAAPI by default for GitHub Runner builds. Set
        # ENABLE_LINUX_VAAPI=0/false to force a CPU-only build if CI dependencies
        # are temporarily unavailable.
        if [[ "$ENABLE_LINUX_VAAPI" != "0" && "$ENABLE_LINUX_VAAPI" != "false" ]]; then
            HWACCEL_CONFIG+=(
                --enable-vaapi
                --enable-hwaccel=h264_vaapi
                --enable-hwaccel=hevc_vaapi
            )
        fi
        ;;
    msys*|cygwin*|win32)
        HWACCEL_CONFIG+=(
            --enable-d3d11va
            --enable-dxva2
            --enable-hwaccel=h264_d3d11va
            --enable-hwaccel=h264_d3d11va2
            --enable-hwaccel=hevc_d3d11va
            --enable-hwaccel=hevc_d3d11va2
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
            $JNI_INCLUDES -I"$INSTALL_DIR/include" \
            -Wl,--no-undefined -Wl,-rpath,'$ORIGIN' $LIBS $EXTRA_FLAGS
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
            $JNI_INCLUDES -I"$INSTALL_DIR/include" \
            -Wl,--no-undefined -Wl,-rpath,'$ORIGIN' $LIBS_VIDEO $EXTRA_FLAGS
        ;;
    msys*|cygwin*|win32)
        gcc -shared -o "$INSTALL_DIR/bin/video_jni.dll" "$SCRIPT_DIR/video_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS_VIDEO \
            -Wl,--out-implib,libvideo_jni.dll.a $EXTRA_FLAGS
        ;;
esac

if [[ "$OSTYPE" == darwin* ]]; then
    echo "=== macOS Mach-O install name fix ==="
    # Local build writes JNI dylibs to bin for historical compatibility; keep Java-facing
    # files together with FFmpeg libs when auditing/fixing loader-relative dependencies.
    cp -f "$INSTALL_DIR/bin/libeac3_jni.dylib" "$INSTALL_DIR/lib/libeac3_jni.dylib"
    cp -f "$INSTALL_DIR/bin/libvideo_jni.dylib" "$INSTALL_DIR/lib/libvideo_jni.dylib"

    install_name_tool -id "@loader_path/libavutil.60.dylib" "$INSTALL_DIR/lib/libavutil.60.dylib"
    install_name_tool -id "@loader_path/libswresample.6.dylib" "$INSTALL_DIR/lib/libswresample.6.dylib"
    install_name_tool -id "@loader_path/libswscale.9.dylib" "$INSTALL_DIR/lib/libswscale.9.dylib"
    install_name_tool -id "@loader_path/libavcodec.62.dylib" "$INSTALL_DIR/lib/libavcodec.62.dylib"
    install_name_tool -id "@loader_path/libeac3_jni.dylib" "$INSTALL_DIR/lib/libeac3_jni.dylib"
    install_name_tool -id "@loader_path/libvideo_jni.dylib" "$INSTALL_DIR/lib/libvideo_jni.dylib"

    for lib in libswresample.6.dylib libswscale.9.dylib libavcodec.62.dylib libeac3_jni.dylib libvideo_jni.dylib; do
        install_name_tool -change "$INSTALL_DIR/lib/libavutil.60.dylib" \
            "@loader_path/libavutil.60.dylib" "$INSTALL_DIR/lib/$lib" || true
    done
    for lib in libeac3_jni.dylib libvideo_jni.dylib; do
        install_name_tool -change "$INSTALL_DIR/lib/libavcodec.62.dylib" \
            "@loader_path/libavcodec.62.dylib" "$INSTALL_DIR/lib/$lib" || true
    done
    install_name_tool -change "$INSTALL_DIR/lib/libswscale.9.dylib" \
        "@loader_path/libswscale.9.dylib" "$INSTALL_DIR/lib/libvideo_jni.dylib" || true

    for lib in libavutil.60.dylib libswresample.6.dylib libswscale.9.dylib libavcodec.62.dylib libeac3_jni.dylib libvideo_jni.dylib; do
        if otool -L "$INSTALL_DIR/lib/$lib" | grep -q "$INSTALL_DIR/lib"; then
            echo "ERROR: $lib still contains absolute install path" >&2
            otool -L "$INSTALL_DIR/lib/$lib" >&2
            exit 1
        fi
    done

    cp -f "$INSTALL_DIR/lib/libeac3_jni.dylib" "$INSTALL_DIR/bin/libeac3_jni.dylib"
    cp -f "$INSTALL_DIR/lib/libvideo_jni.dylib" "$INSTALL_DIR/bin/libvideo_jni.dylib"
fi

if [[ "$OSTYPE" == linux* ]]; then
    echo "=== Linux ELF symbol audit ==="
    require_export() {
        local lib="$1" symbol="$2"
        if ! readelf --dyn-syms --wide "$INSTALL_DIR/lib/$lib" | grep -q " $symbol@@\| $symbol$"; then
            echo "ERROR: $lib does not export required dynamic symbol: $symbol" >&2
            readelf -d "$INSTALL_DIR/lib/$lib" || true
            readelf --dyn-syms --wide "$INSTALL_DIR/lib/$lib" | head -80 || true
            exit 1
        fi
    }
    require_needed() {
        local lib="$1" needed="$2"
        if ! readelf -d "$INSTALL_DIR/lib/$lib" | grep -q "Shared library: \[$needed"; then
            echo "ERROR: $lib is missing DT_NEEDED entry for $needed" >&2
            readelf -d "$INSTALL_DIR/lib/$lib" || true
            objdump -T "$INSTALL_DIR/lib/$lib" | grep -E 'avcodec|avutil|sws_' || true
            exit 1
        fi
    }
    require_export libavutil.so.60 av_buffer_allocz
    require_export libavcodec.so.62 avcodec_send_packet
    require_export libswscale.so.9 sws_scale
    require_needed libeac3_jni.so libavcodec.so.62
    require_needed libeac3_jni.so libavutil.so.60
    require_needed libvideo_jni.so libavcodec.so.62
    require_needed libvideo_jni.so libavutil.so.60
    require_needed libvideo_jni.so libswscale.so.9
fi

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

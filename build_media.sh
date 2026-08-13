#!/bin/bash
# 构建最小 FFmpeg + JNI: E-AC-3 音频 + H.264/AV1 视频
# 需要: gcc/clang, make, nasm, upx (可选), JDK
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="$SCRIPT_DIR/ffmpeg_install"

ffmpeg_major() {
    awk '/#define[[:space:]]+'"$1"'[[:space:]]+/ { print $3; exit }' "$SCRIPT_DIR/$2"
}

AVUTIL_MAJOR="$(ffmpeg_major LIBAVUTIL_VERSION_MAJOR libavutil/version.h)"
SWSCALE_MAJOR="$(ffmpeg_major LIBSWSCALE_VERSION_MAJOR libswscale/version_major.h)"
AVCODEC_MAJOR="$(ffmpeg_major LIBAVCODEC_VERSION_MAJOR libavcodec/version_major.h)"

echo "=== FFmpeg minimal media build (E-AC-3 + H.264 + AV1) ==="

# dav1d is built separately as a pinned static library. Keeping it out of the
# runtime file set avoids a seventh platform-specific loader dependency while
# still providing an explicit SOFTWARE_ONLY AV1 backend.
DAV1D_PREFIX="${DAV1D_PREFIX:-$SCRIPT_DIR/dav1d_install}"
DAV1D_LICENSE_FILE="${DAV1D_LICENSE_FILE:-$SCRIPT_DIR/third_party/dav1d/COPYING}"
DAV1D_PC="$DAV1D_PREFIX/lib/pkgconfig/dav1d.pc"
if [ ! -s "$DAV1D_PC" ]; then
    echo "ERROR: pinned static dav1d pkg-config file is missing: $DAV1D_PC" >&2
    exit 1
fi
if [ ! -s "$DAV1D_LICENSE_FILE" ]; then
    echo "ERROR: dav1d BSD-2-Clause license is missing: $DAV1D_LICENSE_FILE" >&2
    exit 1
fi
export PKG_CONFIG_PATH="$DAV1D_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [ "$(pkg-config --modversion dav1d)" != "1.5.4" ]; then
    echo "ERROR: expected dav1d 1.5.4, got $(pkg-config --modversion dav1d)" >&2
    exit 1
fi

HWACCEL_CONFIG=()
TOOLCHAIN_CONFIG=()
ENABLE_LINUX_VAAPI="${ENABLE_LINUX_VAAPI:-1}"
case "$OSTYPE" in
    darwin*)
        TOOLCHAIN_CONFIG+=(--enable-pthreads)
        HWACCEL_CONFIG+=(
            --enable-hwaccel=h264_videotoolbox
            --enable-hwaccel=av1_videotoolbox
        )
        ;;
    linux*)
        TOOLCHAIN_CONFIG+=(--enable-pthreads)
        # Linux bundle enables VAAPI by default for GitHub Runner builds. Set
        # ENABLE_LINUX_VAAPI=0/false to force a CPU-only build if CI dependencies
        # are temporarily unavailable.
        if [[ "$ENABLE_LINUX_VAAPI" != "0" && "$ENABLE_LINUX_VAAPI" != "false" ]]; then
            HWACCEL_CONFIG+=(
                --enable-vaapi
                --enable-hwaccel=h264_vaapi
                --enable-hwaccel=av1_vaapi
            )
        fi
        ;;
    msys*|cygwin*|win32)
        TOOLCHAIN_CONFIG+=(
            --enable-w32threads
            --disable-pthreads
            --extra-ldflags="-static-libgcc -static-libstdc++"
        )
        HWACCEL_CONFIG+=(
            --enable-d3d11va
            --enable-dxva2
            --enable-hwaccel=h264_d3d11va
            --enable-hwaccel=h264_d3d11va2
            --enable-hwaccel=av1_d3d11va
            --enable-hwaccel=av1_d3d11va2
            --enable-hwaccel=h264_dxva2
            --enable-hwaccel=av1_dxva2
        )
        ;;
esac

# ── 1. 构建 FFmpeg ──
./configure \
    --disable-everything \
    --enable-decoder=eac3 \
    --enable-decoder=h264 \
    --enable-decoder=av1 \
    --enable-decoder=libdav1d \
    --enable-libdav1d \
    --enable-parser=ac3 \
    --enable-parser=h264 \
    --enable-parser=av1 \
    --enable-bsf=h264_mp4toannexb \
    --enable-swscale \
    --disable-swresample \
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
    --pkg-config-flags="--static" \
    --prefix="$INSTALL_DIR" \
    "${TOOLCHAIN_CONFIG[@]}" \
    "${HWACCEL_CONFIG[@]}"

grep -q '^#define CONFIG_AV1_DECODER 1$' config_components.h
grep -q '^#define CONFIG_LIBDAV1D_DECODER 1$' config_components.h

make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
make install
cp "$DAV1D_LICENSE_FILE" "$INSTALL_DIR/Dav1d-BSD-2-Clause.txt"

if find "$INSTALL_DIR" -type f \( -iname '*swresample*' -o -iname '*swresample-*' \) | grep -q .; then
    echo "ERROR: libswresample was produced despite --disable-swresample" >&2
    find "$INSTALL_DIR" -type f \( -iname '*swresample*' -o -iname '*swresample-*' \) -print >&2
    exit 1
fi

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
CC_BIN="${CC:-gcc}"
EXTRA_FLAGS=()
case "$OSTYPE" in
    msys*|cygwin*|win32) EXTRA_FLAGS+=( -static-libgcc ) ;;
esac

case "$OSTYPE" in
    darwin*)
        "$CC_BIN" -shared -o "$INSTALL_DIR/bin/libeac3_jni.dylib" "$SCRIPT_DIR/eac3_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS "${EXTRA_FLAGS[@]}"
        ;;
    linux*)
        "$CC_BIN" -shared -o "$INSTALL_DIR/lib/libeac3_jni.so" "$SCRIPT_DIR/eac3_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" \
            -Wl,--no-undefined -Wl,-rpath,'$ORIGIN' $LIBS "${EXTRA_FLAGS[@]}"
        ;;
    msys*|cygwin*|win32)
        "$CC_BIN" -shared -o "$INSTALL_DIR/bin/eac3_jni.dll" "$SCRIPT_DIR/eac3_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS \
            -Wl,--out-implib,libeac3_jni.dll.a "${EXTRA_FLAGS[@]}"
        ;;
esac

# ── 3. 构建 video_jni (视频) ──
echo "=== build video_jni ==="
LIBS_VIDEO="$LIBS -lswscale"

case "$OSTYPE" in
    darwin*)
        "$CC_BIN" -shared -o "$INSTALL_DIR/bin/libvideo_jni.dylib" "$SCRIPT_DIR/video_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS_VIDEO "${EXTRA_FLAGS[@]}"
        ;;
    linux*)
        "$CC_BIN" -shared -o "$INSTALL_DIR/lib/libvideo_jni.so" "$SCRIPT_DIR/video_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" \
            -Wl,--no-undefined -Wl,-rpath,'$ORIGIN' $LIBS_VIDEO "${EXTRA_FLAGS[@]}"
        ;;
    msys*|cygwin*|win32)
        "$CC_BIN" -shared -o "$INSTALL_DIR/bin/video_jni.dll" "$SCRIPT_DIR/video_jni.c" \
            $JNI_INCLUDES -I"$INSTALL_DIR/include" $LIBS_VIDEO \
            -Wl,--out-implib,libvideo_jni.dll.a "${EXTRA_FLAGS[@]}"
        ;;
esac

if [[ "$OSTYPE" == darwin* ]]; then
    echo "=== macOS Mach-O install name fix ==="
    # Local build writes JNI dylibs to bin for historical compatibility; keep Java-facing
    # files together with FFmpeg libs when auditing/fixing loader-relative dependencies.
    cp -f "$INSTALL_DIR/bin/libeac3_jni.dylib" "$INSTALL_DIR/lib/libeac3_jni.dylib"
    cp -f "$INSTALL_DIR/bin/libvideo_jni.dylib" "$INSTALL_DIR/lib/libvideo_jni.dylib"

    ensure_dylib_name() {
        local short="$1"
        if [ -f "$INSTALL_DIR/lib/$short" ]; then
            return 0
        fi
        local stem="${short%.dylib}"
        local candidate
        candidate="$(find "$INSTALL_DIR/lib" -maxdepth 1 -type f -name "$stem.*.dylib" | sort | head -n 1 || true)"
        if [ -n "$candidate" ]; then
            cp "$candidate" "$INSTALL_DIR/lib/$short"
            return 0
        fi
        local link_target
        link_target="$(find "$INSTALL_DIR/lib" -maxdepth 1 -type l -name "$stem*.dylib" | sort | head -n 1 || true)"
        if [ -n "$link_target" ]; then
            cp "$(readlink "$link_target")" "$INSTALL_DIR/lib/$short" 2>/dev/null || cp "$link_target" "$INSTALL_DIR/lib/$short"
        fi
    }

    AVUTIL_DYLIB="libavutil.${AVUTIL_MAJOR}.dylib"
    SWSCALE_DYLIB="libswscale.${SWSCALE_MAJOR}.dylib"
    AVCODEC_DYLIB="libavcodec.${AVCODEC_MAJOR}.dylib"

    for lib in "$AVUTIL_DYLIB" "$SWSCALE_DYLIB" "$AVCODEC_DYLIB"; do
        ensure_dylib_name "$lib"
    done

    install_name_tool -id "@loader_path/$AVUTIL_DYLIB" "$INSTALL_DIR/lib/$AVUTIL_DYLIB"
    install_name_tool -id "@loader_path/$SWSCALE_DYLIB" "$INSTALL_DIR/lib/$SWSCALE_DYLIB"
    install_name_tool -id "@loader_path/$AVCODEC_DYLIB" "$INSTALL_DIR/lib/$AVCODEC_DYLIB"
    install_name_tool -id "@loader_path/libeac3_jni.dylib" "$INSTALL_DIR/lib/libeac3_jni.dylib"
    install_name_tool -id "@loader_path/libvideo_jni.dylib" "$INSTALL_DIR/lib/libvideo_jni.dylib"

    for lib in "$SWSCALE_DYLIB" "$AVCODEC_DYLIB" libeac3_jni.dylib libvideo_jni.dylib; do
        install_name_tool -change "$INSTALL_DIR/lib/$AVUTIL_DYLIB" \
            "@loader_path/$AVUTIL_DYLIB" "$INSTALL_DIR/lib/$lib" || true
    done
    for lib in libeac3_jni.dylib libvideo_jni.dylib; do
        install_name_tool -change "$INSTALL_DIR/lib/$AVCODEC_DYLIB" \
            "@loader_path/$AVCODEC_DYLIB" "$INSTALL_DIR/lib/$lib" || true
    done
    install_name_tool -change "$INSTALL_DIR/lib/$SWSCALE_DYLIB" \
        "@loader_path/$SWSCALE_DYLIB" "$INSTALL_DIR/lib/libvideo_jni.dylib" || true

    for lib in "$AVUTIL_DYLIB" "$SWSCALE_DYLIB" "$AVCODEC_DYLIB" libeac3_jni.dylib libvideo_jni.dylib; do
        if otool -L "$INSTALL_DIR/lib/$lib" | grep -q "$INSTALL_DIR/lib"; then
            echo "ERROR: $lib still contains absolute install path" >&2
            otool -L "$INSTALL_DIR/lib/$lib" >&2
            exit 1
        fi
    done
    if otool -L "$INSTALL_DIR/lib/$AVCODEC_DYLIB" | grep -qi 'dav1d'; then
        echo "ERROR: libavcodec must contain static dav1d, not a dav1d dylib dependency" >&2
        exit 1
    fi

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
    require_any_export() {
        local lib="$1"
        shift
        local symbol
        for symbol in "$@"; do
            if readelf --dyn-syms --wide "$INSTALL_DIR/lib/$lib" | grep -q " $symbol@@\| $symbol$"; then
                return 0
            fi
        done
        echo "ERROR: $lib does not export any required dynamic symbols: $*" >&2
        readelf -d "$INSTALL_DIR/lib/$lib" || true
        readelf --dyn-syms --wide "$INSTALL_DIR/lib/$lib" | head -80 || true
        exit 1
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
    AVUTIL_SO="libavutil.so.${AVUTIL_MAJOR}"
    SWSCALE_SO="libswscale.so.${SWSCALE_MAJOR}"
    AVCODEC_SO="libavcodec.so.${AVCODEC_MAJOR}"

    require_any_export "$AVUTIL_SO" av_version_info av_frame_alloc av_frame_free
    require_export "$AVCODEC_SO" avcodec_send_packet
    require_export "$SWSCALE_SO" sws_scale
    require_needed libeac3_jni.so "$AVCODEC_SO"
    require_needed libeac3_jni.so "$AVUTIL_SO"
    require_needed libvideo_jni.so "$AVCODEC_SO"
    require_needed libvideo_jni.so "$AVUTIL_SO"
    require_needed libvideo_jni.so "$SWSCALE_SO"
    if readelf -d "$INSTALL_DIR/lib/$AVCODEC_SO" | grep -qi 'dav1d'; then
        echo "ERROR: libavcodec must contain static dav1d, not a libdav1d DT_NEEDED entry" >&2
        exit 1
    fi
fi

if [[ "$OSTYPE" == msys* || "$OSTYPE" == cygwin* || "$OSTYPE" == win32 ]]; then
    AVCODEC_DLL="$(find "$INSTALL_DIR/bin" -maxdepth 1 -type f -iname 'avcodec-*.dll' | head -n 1)"
    if objdump -p "$AVCODEC_DLL" | grep -qi 'DLL Name:.*dav1d'; then
        echo "ERROR: avcodec DLL must contain static dav1d, not import dav1d.dll" >&2
        exit 1
    fi
fi

# ── 4. 报告 ──
echo ""
echo "=== Build complete ==="
case "$OSTYPE" in
    darwin*)
        LIB_DIR="$INSTALL_DIR/bin"
        LIBS_LIST=("libavutil.${AVUTIL_MAJOR}" "libswscale.${SWSCALE_MAJOR}" "libavcodec.${AVCODEC_MAJOR}" "libeac3_jni" "libvideo_jni")
        EXT=".dylib"
        ;;
    linux*)
        LIB_DIR="$INSTALL_DIR/lib"
        LIBS_LIST=("libavutil.so.${AVUTIL_MAJOR}" "libswscale.so.${SWSCALE_MAJOR}" "libavcodec.so.${AVCODEC_MAJOR}" "libeac3_jni" "libvideo_jni")
        EXT=".so"
        ;;
    msys*|cygwin*|win32)
        LIB_DIR="$INSTALL_DIR/bin"
        LIBS_LIST=("avutil-${AVUTIL_MAJOR}" "swscale-${SWSCALE_MAJOR}" "avcodec-${AVCODEC_MAJOR}" "eac3_jni" "video_jni")
        EXT=".dll"
        ;;
esac

for lib in "${LIBS_LIST[@]}"; do
    if [[ "$lib" == *.so.* ]]; then
        path="$LIB_DIR/$lib"
    else
        path="$LIB_DIR/${lib}${EXT}"
    fi
    if [ -f "$path" ]; then
        size=$(stat -c%s "$path" 2>/dev/null || stat -f%z "$path" 2>/dev/null)
        printf "  %-40s %6d bytes\n" "$(basename "$path")" "$size"
    else
        printf "  %-40s MISSING\n" "$(basename "$path")"
    fi
done

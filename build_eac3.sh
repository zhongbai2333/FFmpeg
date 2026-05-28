#!/bin/bash
# 本地构建最小 E-AC-3 FFmpeg
# 需要: gcc/clang, make, nasm, upx (可选)
set -euo pipefail

echo "=== FFmpeg minimal E-AC-3 build ==="

./configure \
    --disable-everything \
    --enable-decoder=eac3 \
    --enable-parser=ac3 \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avformat \
    --disable-swscale \
    --disable-avfilter \
    --disable-network \
    --disable-encoders \
    --disable-muxers \
    --disable-protocols \
    --disable-filters \
    --enable-small \
    --disable-debug \
    --enable-shared \
    --prefix="$PWD/install"

make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
make install

# Windows / Linux: UPX 压缩
# macOS: 跳过，避免破坏代码签名
if [[ "$OSTYPE" != "darwin"* ]] && command -v upx &>/dev/null; then
    echo "=== UPX compress ==="
    find install -type f \( -name "*.dll" -o -name "*.so*" \) | while read f; do
        upx -9 "$f" || true
    done
fi

echo "=== Done ==="
find install -type f \( -name "*.dll" -o -name "*.dylib" -o -name "*.so*" \) | while read f; do
    size=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null)
    printf "  %-40s %6d bytes\n" "$(basename "$f")" "$size"
done

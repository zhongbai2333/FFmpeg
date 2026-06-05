#!/usr/bin/env bash
# Build tiny FFmpeg variants and report native library size deltas.
# Intended for GitHub Actions, but can also be run locally from the FFmpeg repo root.
set -euo pipefail

VARIANT="${1:-h264}"
PREFIX="${2:-$PWD/install-$VARIANT}"
REPORT="${3:-$PWD/size-report-$VARIANT.txt}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

case "$VARIANT" in
  eac3|h264|hevc)
    ;;
  *)
    echo "usage: $0 {eac3|h264|hevc} [prefix] [report]" >&2
    exit 2
    ;;
esac

rm -rf "$PREFIX" "$REPORT"
mkdir -p "$PREFIX"

CONFIG=(
  --disable-everything
  --enable-decoder=eac3
  --enable-parser=ac3
  --disable-programs
  --disable-doc
  --disable-avdevice
  --disable-avformat
  --disable-avfilter
  --disable-network
  --disable-encoders
  --disable-muxers
  --disable-protocols
  --disable-filters
  --enable-small
  --disable-debug
  --enable-shared
  --prefix="$PREFIX"
)

case "$VARIANT" in
  eac3)
    CONFIG+=(--disable-swscale)
    ;;
  h264)
    CONFIG+=(
      --enable-decoder=h264
      --enable-parser=h264
      --enable-bsf=h264_mp4toannexb
      --enable-swscale
    )
    ;;
  hevc)
    CONFIG+=(
      --enable-decoder=h264
      --enable-decoder=hevc
      --enable-parser=h264
      --enable-parser=hevc
      --enable-bsf=h264_mp4toannexb
      --enable-bsf=hevc_mp4toannexb
      --enable-swscale
    )
    ;;
esac

# Keep platform/toolchain knobs outside the script so the workflow matrix can tune them.
if [[ -n "${EXTRA_CONFIG:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_CONFIG_ARRAY=(${EXTRA_CONFIG})
  CONFIG+=("${EXTRA_CONFIG_ARRAY[@]}")
fi

{
  echo "variant=$VARIANT"
  echo "prefix=$PREFIX"
  echo "uname=$(uname -a 2>/dev/null || true)"
  echo "extra_config=${EXTRA_CONFIG:-}"
  echo "configure=${CONFIG[*]}"
} > "$REPORT"

./configure "${CONFIG[@]}"
make -j"$JOBS"
make install

if [[ "${UPX:-false}" == "true" && "$(uname -s 2>/dev/null || true)" != Linux* ]] && command -v upx >/dev/null 2>&1; then
  find "$PREFIX" -type f \( -name "*.dll" -o -name "*.so*" \) | while read -r f; do
    case "$(basename "$f")" in
      libwinpthread-1.dll) continue ;;
    esac
    upx -9 "$f" >/dev/null 2>&1 || true
  done
fi

{
  echo
  echo "files:"
  find "$PREFIX" -type f \( -name "*.dll" -o -name "*.dylib" -o -name "*.so" -o -name "*.so.*" \) -print0 \
    | sort -z \
    | while IFS= read -r -d '' f; do
        size=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null)
        printf "%12d  %s\n" "$size" "${f#$PREFIX/}"
      done
  echo
  total=$(find "$PREFIX" -type f \( -name "*.dll" -o -name "*.dylib" -o -name "*.so" -o -name "*.so.*" \) -print0 \
    | while IFS= read -r -d '' f; do stat -c%s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null; done \
    | awk '{s+=$1} END {print s+0}')
  printf "total_bytes=%d\n" "$total"
  awk -v s="$total" 'BEGIN {printf "total_mib=%.3f\n", s/1024/1024}'
} >> "$REPORT"

cat "$REPORT"

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-musl-static}"
WITH_VIDEO="${WITH_VIDEO:-0}"
WITH_VIDEO_DEVICE="${WITH_VIDEO_DEVICE:-0}"
FFMPEG_STATIC_PREFIX="${FFMPEG_STATIC_PREFIX:-/opt/ffmpeg-static}"
TURBOJPEG_MODE="${TURBOJPEG_MODE:-auto}"
TURBOJPEG_STATIC_PREFIX="${TURBOJPEG_STATIC_PREFIX:-/opt/turbojpeg-static}"

# Keep this build mostly minimal to avoid pulling many static third-party libraries.
CMAKE_OPTS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/musl-static-minimal.cmake
  -DWITH_OPENSLIDE_SUPPORT=Off
  -DWITH_GRAPHICSMAGICK=Off
  -DWITH_RSVG=Off
  -DWITH_POPPLER=Off
  -DWITH_LIBSIXEL=Off
  -DWITH_STB_IMAGE=On
)

if [ "${WITH_VIDEO}" = "1" ]; then
  CMAKE_OPTS+=(-DWITH_VIDEO_DECODING=On)
  CMAKE_OPTS+=(-DCMAKE_PREFIX_PATH="${FFMPEG_STATIC_PREFIX}")
  if [ "${WITH_VIDEO_DEVICE}" = "1" ]; then
    CMAKE_OPTS+=(-DWITH_VIDEO_DEVICE=On)
  else
    CMAKE_OPTS+=(-DWITH_VIDEO_DEVICE=Off)
  fi
else
  CMAKE_OPTS+=(-DWITH_VIDEO_DECODING=Off -DWITH_VIDEO_DEVICE=Off)
fi

if command -v x86_64-linux-musl-g++ >/dev/null 2>&1; then
  HAS_STATIC_TURBOJPEG=0
  if find /usr/lib /usr/local/lib -name libturbojpeg.a -print -quit 2>/dev/null | grep -q . \
    && find /usr/lib /usr/local/lib -name libexif.a -print -quit 2>/dev/null | grep -q .; then
    HAS_STATIC_TURBOJPEG=1
  fi
  case "${TURBOJPEG_MODE}" in
    on|ON)
      if [ "${HAS_STATIC_TURBOJPEG}" -ne 1 ]; then
        echo "TURBOJPEG_MODE=on requested, but static libturbojpeg.a/libexif.a not found" >&2
        exit 2
      fi
      CMAKE_OPTS+=(-DWITH_TURBOJPEG=On)
      ;;
    off|OFF)
      CMAKE_OPTS+=(-DWITH_TURBOJPEG=Off)
      ;;
    auto|AUTO)
      if [ "${HAS_STATIC_TURBOJPEG}" -eq 1 ]; then
        CMAKE_OPTS+=(-DWITH_TURBOJPEG=On)
      else
        CMAKE_OPTS+=(-DWITH_TURBOJPEG=Off)
      fi
      ;;
    *)
      echo "Invalid TURBOJPEG_MODE='${TURBOJPEG_MODE}'. Use auto|on|off." >&2
      exit 2
      ;;
  esac

  echo "Building with local musl toolchain into ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${CMAKE_OPTS[@]}" \
    -DCMAKE_C_COMPILER=x86_64-linux-musl-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-linux-musl-g++
  cmake --build "${BUILD_DIR}" -j"$(nproc)"
else
  echo "x86_64-linux-musl-g++ not found; building in Alpine Docker instead"
  CMAKE_OPTS_STR="$(printf '%q ' "${CMAKE_OPTS[@]}")"
  docker run --rm \
    -e HOST_UID="$(id -u)" \
    -e HOST_GID="$(id -g)" \
    -e TURBOJPEG_MODE="${TURBOJPEG_MODE}" \
    -e TURBOJPEG_STATIC_PREFIX="${TURBOJPEG_STATIC_PREFIX}" \
    -v "${ROOT_DIR}":/src \
    -w /src \
    alpine:3.20 \
    sh -euxc '
      APK_PKGS="build-base cmake pkgconf git libdeflate-dev libdeflate-static"
      if [ "'"${WITH_VIDEO}"'" = "1" ]; then
        APK_PKGS="${APK_PKGS} nasm yasm"
      fi
      if [ "${TURBOJPEG_MODE}" != "off" ] && [ "${TURBOJPEG_MODE}" != "OFF" ]; then
        APK_PKGS="${APK_PKGS} autoconf automake gettext gettext-dev libtool libjpeg-turbo-dev libjpeg-turbo-static libexif-dev"
        if apk search -x libexif-static 2>/dev/null | grep -qE '^libexif-static-[0-9]'; then
          APK_PKGS="${APK_PKGS} libexif-static"
        fi
      fi
      apk add --no-cache \
        ${APK_PKGS}

      HAS_STATIC_TURBOJPEG=0
      if [ -f /usr/lib/libturbojpeg.a ] && [ -f /usr/lib/libexif.a ]; then
        HAS_STATIC_TURBOJPEG=1
      fi

      JPEG_PREFIX="${TURBOJPEG_STATIC_PREFIX}"
      if [ "${TURBOJPEG_MODE}" != "off" ] && [ "${TURBOJPEG_MODE}" != "OFF" ] && [ "${HAS_STATIC_TURBOJPEG}" -ne 1 ]; then
        echo "Building static turbojpeg/exif dependencies into ${JPEG_PREFIX}"
        rm -rf "${JPEG_PREFIX}" /tmp/libjpeg-turbo-src /tmp/libjpeg-turbo-build /tmp/libexif-src /tmp/libexif-build
        mkdir -p "${JPEG_PREFIX}" /tmp/libjpeg-turbo-src /tmp/libjpeg-turbo-build /tmp/libexif-src /tmp/libexif-build

        wget -qO- https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.0.3.tar.gz \
          | tar xz -C /tmp/libjpeg-turbo-src --strip-components=1
        cmake -S /tmp/libjpeg-turbo-src -B /tmp/libjpeg-turbo-build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX="${JPEG_PREFIX}" \
          -DCMAKE_INSTALL_LIBDIR=lib \
          -DENABLE_SHARED=OFF \
          -DENABLE_STATIC=ON
        cmake --build /tmp/libjpeg-turbo-build -j"$(getconf _NPROCESSORS_ONLN)"
        cmake --install /tmp/libjpeg-turbo-build

        LIBEXIF_TARBALL=/tmp/libexif.tar.xz
        for libexif_url in \
          https://deb.debian.org/debian/pool/main/libe/libexif/libexif_0.6.25.orig.tar.gz \
          https://deb.debian.org/debian/pool/main/libe/libexif/libexif_0.6.24.orig.tar.gz; do
          if wget -qO "${LIBEXIF_TARBALL}" "${libexif_url}"; then
            break
          fi
        done
        if [ ! -s "${LIBEXIF_TARBALL}" ]; then
          echo "Failed to download libexif source tarball." >&2
          exit 2
        fi
        tar xf "${LIBEXIF_TARBALL}" -C /tmp/libexif-src --strip-components=1
        cd /tmp/libexif-src
        autoreconf -fi
        cd /tmp/libexif-build
        /tmp/libexif-src/configure \
          --prefix="${JPEG_PREFIX}" \
          --enable-static \
          --disable-nls \
          --disable-shared
        make -j"$(getconf _NPROCESSORS_ONLN)"
        make install
        cd /src

        if [ -f "${JPEG_PREFIX}/lib/libturbojpeg.a" ] && [ -f "${JPEG_PREFIX}/lib/libexif.a" ]; then
          HAS_STATIC_TURBOJPEG=1
        fi
      fi
      case "${TURBOJPEG_MODE}" in
        on|ON)
          if [ "${HAS_STATIC_TURBOJPEG}" -ne 1 ]; then
            echo "TURBOJPEG_MODE=on requested, but static libturbojpeg.a/libexif.a not found" >&2
            exit 2
          fi
          TURBOJPEG_CMAKE_ARG="-DWITH_TURBOJPEG=On"
          ;;
        off|OFF)
          TURBOJPEG_CMAKE_ARG="-DWITH_TURBOJPEG=Off"
          ;;
        auto|AUTO)
          if [ "${HAS_STATIC_TURBOJPEG}" -eq 1 ]; then
            TURBOJPEG_CMAKE_ARG="-DWITH_TURBOJPEG=On"
          else
            TURBOJPEG_CMAKE_ARG="-DWITH_TURBOJPEG=Off"
          fi
          ;;
        *)
          echo "Invalid TURBOJPEG_MODE=${TURBOJPEG_MODE}. Use auto|on|off." >&2
          exit 2
          ;;
      esac

      CMAKE_PREFIX_PATH_VALUE=""
      if [ "'"${WITH_VIDEO}"'" = "1" ]; then
        CMAKE_PREFIX_PATH_VALUE="'"${FFMPEG_STATIC_PREFIX}"'"
      fi
      if [ "${HAS_STATIC_TURBOJPEG}" -eq 1 ]; then
        if [ -n "${CMAKE_PREFIX_PATH_VALUE}" ]; then
          CMAKE_PREFIX_PATH_VALUE="${CMAKE_PREFIX_PATH_VALUE};${JPEG_PREFIX}"
        else
          CMAKE_PREFIX_PATH_VALUE="${JPEG_PREFIX}"
        fi
      fi

      if [ "'"${WITH_VIDEO}"'" = "1" ]; then
        FFMPEG_PREFIX="'"${FFMPEG_STATIC_PREFIX}"'"
        rm -rf /tmp/ffmpeg-src "${FFMPEG_PREFIX}"
        git clone --depth=1 --branch n6.1.1 https://github.com/FFmpeg/FFmpeg.git /tmp/ffmpeg-src
        cd /tmp/ffmpeg-src
        if [ "'"${WITH_VIDEO_DEVICE}"'" = "1" ]; then
          VIDEO_DEVICE_ARG="--enable-avdevice"
        else
          VIDEO_DEVICE_ARG="--disable-avdevice"
        fi
        ./configure \
          --prefix="${FFMPEG_PREFIX}" \
          --enable-static \
          --disable-shared \
          --disable-programs \
          --disable-doc \
          --disable-debug \
          --disable-network \
          --disable-postproc \
          --disable-avfilter \
          --disable-autodetect \
          ${VIDEO_DEVICE_ARG}
        make -j"$(getconf _NPROCESSORS_ONLN)"
        make install
        cd /src

        for lib in \
          "${FFMPEG_PREFIX}/lib/libavcodec.a" \
          "${FFMPEG_PREFIX}/lib/libavutil.a" \
          "${FFMPEG_PREFIX}/lib/libavformat.a" \
          "${FFMPEG_PREFIX}/lib/libswscale.a"; do
          if [ ! -f "${lib}" ]; then
            echo "Missing static FFmpeg library: ${lib}" >&2
            echo "Failed to build static FFmpeg archives needed for video support." >&2
            exit 2
          fi
        done
        if [ "'"${WITH_VIDEO_DEVICE}"'" = "1" ] && [ ! -f "${FFMPEG_PREFIX}/lib/libavdevice.a" ]; then
          echo "Missing static FFmpeg library: ${FFMPEG_PREFIX}/lib/libavdevice.a" >&2
          exit 2
        fi
      fi
      rm -rf /src/build-musl-static
      if [ -n "${CMAKE_PREFIX_PATH_VALUE}" ]; then
        CMAKE_PREFIX_PATH_ARG="-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH_VALUE}"
        cmake -S /src -B /src/build-musl-static '"${CMAKE_OPTS_STR}"' "${TURBOJPEG_CMAKE_ARG}" "${CMAKE_PREFIX_PATH_ARG}"
      else
        cmake -S /src -B /src/build-musl-static '"${CMAKE_OPTS_STR}"' "${TURBOJPEG_CMAKE_ARG}"
      fi
      cmake --build /src/build-musl-static -j"$(getconf _NPROCESSORS_ONLN)"
      chown -R "${HOST_UID}:${HOST_GID}" /src/build-musl-static
    '
fi

BIN="${BUILD_DIR}/src/timg"
if [ ! -x "${BIN}" ]; then
  echo "Expected binary not found at ${BIN}" >&2
  exit 1
fi

echo
file "${BIN}"
if command -v ldd >/dev/null 2>&1; then
  ldd "${BIN}" || true
fi

echo
echo "Feature summary:"
VERSION_OUT="$(mktemp)"
"${BIN}" --version > "${VERSION_OUT}"
grep -E '^timg |Turbo JPEG|QOI image loading|STB image loading|JPEG loading|Video decoding|Resize:|libdeflate|sixel output|GraphicsMagick|librsvg|OpenSlide|poppler' "${VERSION_OUT}" || true
rm -f "${VERSION_OUT}"

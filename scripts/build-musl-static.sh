#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-musl-static}"

# Keep this build minimal to avoid pulling many static third-party libraries.
CMAKE_OPTS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/musl-static-minimal.cmake
  -DWITH_VIDEO_DECODING=Off
  -DWITH_VIDEO_DEVICE=Off
  -DWITH_OPENSLIDE_SUPPORT=Off
  -DWITH_GRAPHICSMAGICK=Off
  -DWITH_TURBOJPEG=Off
  -DWITH_RSVG=Off
  -DWITH_POPPLER=Off
  -DWITH_LIBSIXEL=Off
)

if command -v x86_64-linux-musl-g++ >/dev/null 2>&1; then
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
    -v "${ROOT_DIR}":/src \
    -w /src \
    alpine:3.20 \
    sh -euxc '
      apk add --no-cache build-base cmake pkgconf git libdeflate-dev libdeflate-static
      rm -rf /src/build-musl-static
      cmake -S /src -B /src/build-musl-static '"${CMAKE_OPTS_STR}"'
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

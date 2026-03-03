#!/bin/bash
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN_ROOT="${1:-$HOME/LicheeRV-Nano-Build/host-tools/gcc/riscv64-linux-musl-x86_64}"
COMPILER="$TOOLCHAIN_ROOT/bin"

echo "=== Building opencv-mobile ==="
cd "$REPO_DIR/opencv-mobile-4.13.0"
mkdir -p build && cd build

export RISCV_ROOT_PATH="$TOOLCHAIN_ROOT"

cmake -DCMAKE_TOOLCHAIN_FILE="$REPO_DIR/toolchains/riscv64-unknown-linux-musl.toolchain.cmake" \
      -DCMAKE_C_FLAGS="-fno-rtti -fno-exceptions -I$REPO_DIR/opencv-mobile-4.13.0/modules/highgui/src" \
      -DCMAKE_CXX_FLAGS="-fno-rtti -fno-exceptions -I$REPO_DIR/opencv-mobile-4.13.0/modules/highgui/src" \
      -DCMAKE_INSTALL_PREFIX=install \
      -DCMAKE_BUILD_TYPE=Release \
      -DWITH_CVI=ON \
      -DCPU_BASELINE="" \
      -DCPU_DISPATCH="" \
      $(cat ../options.txt) \
      -DBUILD_opencv_world=OFF ..

make -j$(nproc) && make install

echo "=== Building MJPEGStream ==="
cd "$REPO_DIR/src"
mkdir -p build && cd build

cmake -DCMAKE_C_COMPILER="$COMPILER/riscv64-unknown-linux-musl-gcc" \
      -DCMAKE_CXX_COMPILER="$COMPILER/riscv64-unknown-linux-musl-g++" \
      ..

make -j$(nproc)

echo "=== Done ==="
echo "Binaries: $REPO_DIR/src/build/CSICapture CSIStream CSIHiResStream"
echo "Deploy: scp -P 2222 CSICapture CSIStream CSIHiResStream root@localhost:/root/"

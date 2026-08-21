#!/bin/bash
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_NAME=$(basename "$SCRIPT_DIR")
BUILD_DIR="$HOME/build/$PROJECT_NAME"

PICO_SDK="$SCRIPT_DIR/tools/pico-sdk-2.2.0"
if [ ! -d "$PICO_SDK" ]; then
    PICO_SDK="${PICO_SDK_PATH:?No pico-sdk found. Set PICO_SDK_PATH or put sdk in tools/pico-sdk-2.2.0}"
fi
TOOLCHAIN_FILE="$PICO_SDK/cmake/preload/toolchains/pico_arm_cortex_m33_gcc.cmake"
TOOLCHAIN_BIN="${PICO_TOOLCHAIN_PATH:-$HOME/Sipeed/tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin}"
LWIP_PATH="${PICO_LWIP_PATH:-$PICO_SDK/lib/lwip}"

if [ ! -f "$LWIP_PATH/src/include/lwip/err.h" ]; then
    echo "NCM build requires Pico SDK lwIP. Initialize $PICO_SDK/lib/lwip or set PICO_LWIP_PATH." >&2
    exit 2
fi

# 确保 build 软链接存在
if [ ! -L "$SCRIPT_DIR/build" ]; then
    mkdir -p "$BUILD_DIR"
    ln -s "$BUILD_DIR" "$SCRIPT_DIR/build"
fi

# NCM 构建依赖 lwIP；每次显式传递路径，避免旧 CMake cache 保留失效的
# 临时依赖路径。配置本身不重新编译目标。
cmake "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DPICO_SDK_PATH="$PICO_SDK" \
    -DPICO_LWIP_PATH="$LWIP_PATH" \
    -DPICO_BOARD=pico2 \
    -DCMAKE_BUILD_TYPE=Release \
    -DPICO_TOOLCHAIN_PATH="$TOOLCHAIN_BIN"

# Windows 挂载盘上的头文件 mtime 不可靠，touch 所有 .c 让 cmake 感知到变更
find "$SCRIPT_DIR/src" -name "*.c" | xargs touch

cmake --build "$BUILD_DIR" -j$(nproc)
echo "Output: $BUILD_DIR/${PROJECT_NAME}.elf.uf2"

#!/bin/bash

set -euo pipefail

set -x

dest=${PWD}/vp_install_dbg
build_dir=./build_dbg
export QQVP_FIRMWARE_REPO_DIR=$(realpath ../firmware-images)
[[ -d ${QQVP_FIRMWARE_REPO_DIR} ]] || exit 3
[[ -f ${QQVP_FIRMWARE_REPO_DIR}/opengl_linux/conf.lua ]] || exit 3

#   -Dgs-cmake_SOURCE_DIR=${PWD}/../cmake-boilerplate \
#   -DCPM_base-components_SOURCE=$(realpath ${PWD}/../base-components) \
#   -DCPM_qup_SOURCE=$(realpath ${PWD}/../qup) \

git remote -v

#/pkg/qct/software/cmake/3.25.1/bin/cmake -G'Unix Makefiles' \
#   -DCMAKE_TOOLCHAIN_FILE=$(realpath ../cmake-boilerplate/cmake/clang-toolchain.cmake) \
# -DGS_ENABLE_AUTO_INIT=ON \

cmake -G'Unix Makefiles' \
    -B ${build_dir} \
    -DLIBQEMU_TARGETS='aarch64;hexagon;riscv32;riscv64' \
    -DGS_ENABLE_CXXLIB_CHECK=OFF \
    -DGS_ENABLE_LIBCXX=OFF \
    -DCMAKE_BUILD_TYPE:STRING=Debug \
    -DCMAKE_INSTALL_PREFIX:STRING=${dest} \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -S ${PWD}
cd ${build_dir}
\time cmake --build ${PWD} -- -j$(nproc)
cmake --build ${PWD} -- install -j$(nproc)

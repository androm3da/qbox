#!/bin/bash

set -euo pipefail

set -x

dest=${PWD}/vp_install_clang
build_dir=./build_clang

export QQVP_FIRMWARE_REPO_DIR=$(realpath ../firmware-images)
[[ -d ${QQVP_FIRMWARE_REPO_DIR} ]] || exit 3
[[ -f ${QQVP_FIRMWARE_REPO_DIR}/opengl_linux/conf.lua ]] || exit 3

#export PATH=/local/mnt/workspace/install/LLVM-19.1.0-rc2-Linux-X64/bin:$PATH

#/pkg/qct/software/cmake/3.20.1/bin/cmake -GNinja \
#   -Dgs-cmake_SOURCE_DIR=${PWD}/../cmake-boilerplate \
#   -DCMAKE_TOOLCHAIN_FILE=$(realpath ../cmake-boilerplate/cmake/clang-toolchain.cmake) \
#   -DCMAKE_FIND_DEBUG_MODE=ON \


export LD=clang
#/pkg/qct/software/cmake/3.20.1/bin/cmake -G'Unix Makefiles' \

cmake -G'Unix Makefiles' \
    -B ${build_dir} \
    -DCMAKE_TOOLCHAIN_FILE=$(realpath ../cmake-boilerplate/cmake/clang-toolchain.cmake) \
    -DGS_ENABLE_LLD=ON \
    -DGS_ENABLE_SANITIZERS=OFF \
    -DGS_ENABLE_LIBCXX=ON \
    -DGS_ENABLE_CXXLIB_CHECK=OFF \
    -DCMAKE_INSTALL_PREFIX:STRING=${dest} \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    ${PWD}
cd ${build_dir}
\time cmake --build ${PWD} -- -j$(nproc)
cmake --build ${PWD} -- install -j$(nproc)

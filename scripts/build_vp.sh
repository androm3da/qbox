#!/bin/bash

set -euo pipefail

set -x

dest=${PWD}/vp_install_rel
build_dir=./build_rel

export QQVP_FIRMWARE_REPO_DIR=$(realpath ../firmware-images)
[[ -d ${QQVP_FIRMWARE_REPO_DIR} ]] || exit 3
[[ -f ${QQVP_FIRMWARE_REPO_DIR}/opengl_linux/conf.lua ]] || exit 3

#   -Dgs-cmake_SOURCE_DIR=${PWD}/../cmake-boilerplate \
#   -DQQVP_FIRMWARE_REPO_DIR=${QQVP_FIRMWARE_REPO_DIR} \
#   -DGS_ENABLE_VIRCLRENDERER=OFF \

git remote -v

/pkg/qct/software/cmake/3.20.1/bin/cmake -G'Unix Makefiles' \
    -B ${build_dir} \
    -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX:STRING=${dest} \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DGS_ENABLE_VIRCLRENDERER=ON \
    -DLIBQEMU_TARGETS='aarch64;hexagon;riscv32;riscv64' \
    -DCTEST_OUTPUT_ON_FAILURE=ON \
    ${PWD}
cd ${build_dir}
\time cmake --build ${PWD} -- -j$(nproc)
cmake --build ${PWD} -- install -j$(nproc)

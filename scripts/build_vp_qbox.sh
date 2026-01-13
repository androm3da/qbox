#!/bin/bash

set -euo pipefail

set -x

dest=${PWD}/vp_install_qbox_dbg
build_dir=./build_qbox_dbg

export QQVP_FIRMWARE_REPO_DIR=$(realpath ../firmware-images)
[[ -d ${QQVP_FIRMWARE_REPO_DIR} ]] || exit 3
[[ -f ${QQVP_FIRMWARE_REPO_DIR}/opengl_linux/conf.lua ]] || exit 3

#   -Dgs-cmake_SOURCE_DIR=${PWD}/../cmake-boilerplate \
#   -DCPM_libqbox_SOURCE=$(realpath ../libqbox) \
#   -DGS_ENABLE_CXXLIB_CHECK=ON \
#   -DGS_ENABLE_LIBCXX=ON \
#   -DGS_ENABLE_VIRCLRENDERER=OFF \

git remote -v

/pkg/qct/software/cmake/3.20.1/bin/cmake -G'Unix Makefiles' \
    -B ${build_dir} \
    -DCMAKE_INSTALL_PREFIX:STRING=${dest} \
    -DCPM_qbox_SOURCE=$(realpath ../qbox) \
    -DCMAKE_BUILD_TYPE:STRING=Debug \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    ${PWD}
cd ${build_dir}
\time cmake --build ${PWD} -- -j$(nproc)
cmake --build ${PWD} -- install -j$(nproc)


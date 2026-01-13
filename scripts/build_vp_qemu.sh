#!/bin/bash

set -euo pipefail

set -x

dest=${PWD}/vp_install_qemu
build_dir=./build_qemu
export QQVP_FIRMWARE_REPO_DIR=$(realpath ../firmware-images)
[[ -d ${QQVP_FIRMWARE_REPO_DIR} ]] || exit 3
[[ -f ${QQVP_FIRMWARE_REPO_DIR}/opengl_linux/conf.lua ]] || exit 3


#   -DGS_ENABLE_CAPSTONE=ON \
#   -DGS_ENABLE_CXXLIB_CHECK=ON \
#   -Dgs-cmake_SOURCE_DIR=${PWD}/../cmake-boilerplate \
git remote -v
#git show
/pkg/qct/software/cmake/3.20.1/bin/cmake -G'Unix Makefiles' \
    -B ${build_dir} \
    -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo \
    -DLIBQEMU_TARGETS:STRING='aarch64;hexagon;xtensa;riscv64;riscv32' \
    -DCMAKE_INSTALL_PREFIX:STRING=${dest} \
    -DCTEST_OUTPUT_ON_FAILURE=ON \
    -DCPM_libqemu_SOURCE=$(realpath ../qemu) \
    ${PWD}
cd ${build_dir}
\time cmake --build ${PWD} -- -j$(nproc)
cmake --build ${PWD} -- install -j$(nproc)
#readelf -d $(find ${dest} -name 'libqemu*.so')
#objdump -t $(find ${dest} -name 'libqemu*.so')|grep slirp

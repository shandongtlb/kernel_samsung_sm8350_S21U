#!/bin/bash

mkdir out

BUILD_CROSS_COMPILE=/root/Clang-11/gcc/bin/aarch64-linux-android-
CLANG_PATH=/root/Clang-11/bin
CLANG_TRIPLE=aarch64-linux-gnu-
KERNEL_MAKE_ENV="DTC_EXT=$(pwd)/tools/dtc CONFIG_BUILD_ARM64_DT_OVERLAY=y"

export ARCH=arm64
export PATH=${CLANG_PATH}:${PATH}

make -j16 -C $(pwd) O=$(pwd)/out $KERNEL_MAKE_ENV CROSS_COMPILE=$BUILD_CROSS_COMPILE CLANG_TRIPLE=$CLANG_TRIPLE \
    CC=clang LD=ld.lld \
    vendor/p3q_chn_openx_defconfig

make -j16 -C $(pwd) O=$(pwd)/out $KERNEL_MAKE_ENV CROSS_COMPILE=$BUILD_CROSS_COMPILE CLANG_TRIPLE=$CLANG_TRIPLE \
    CC=clang LD=ld.lld \
    2>&1 | tee kernel.log

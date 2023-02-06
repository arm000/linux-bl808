#!/bin/bash
set -e

SHELL_DIR=$(cd "$(dirname "$0")"; pwd)
OUT_DIR=$SHELL_DIR/out

CMAKE=$SHELL_DIR/toolchain/cmake/bin/

LINUX_CROSS_PREFIX="ccache ../bl808_linux/toolchain/linux_toolchain/bin/riscv64-unknown-linux-gnu-"
NEWLIB_ELF_CROSS_PREFIX=$SHELL_DIR/toolchain/elf_newlib_toolchain/bin/riscv64-unknown-elf-

export CROSS_COMPILE="ccache ../bl808_linux/toolchain/linux_toolchain/bin/riscv64-unknown-linux-gnu-"

BUILD_TARGET=$1

#LINUX_DIR=linux-5.10.4-808
#LINUX_DIR=../linux-5.10.4-808
LINUX_DIR=.

if [[ ! -e $OUT_DIR ]]; then
    mkdir $OUT_DIR
fi

build_linux()
{
    echo " "
    echo "================ build linux kernel ================"
    cd $SHELL_DIR/$LINUX_DIR
    if [ ! -f .config ]; then
#        make ARCH=riscv CROSS_COMPILE="$LINUX_CROSS_PREFIX" bl808_defconfig
        make ARCH=riscv bl808_defconfig
    fi
#    make ARCH=riscv  CROSS_COMPILE="$LINUX_CROSS_PREFIX" Image -j$(nproc)
#    make ARCH=riscv CROSS_COMPILE="$LINUX_CROSS_PREFIX" dtbs -j$(nproc)
     make ARCH=riscv Image -j$(nproc)
     make ARCH=riscv dtbs -j$(nproc)
     echo " "
    echo "=========== high compression kernel image =========="
    lz4 -9 -f $SHELL_DIR/$LINUX_DIR/arch/riscv/boot/Image $SHELL_DIR/$LINUX_DIR/arch/riscv/boot/Image.lz4
    cp $SHELL_DIR/$LINUX_DIR/arch/riscv/boot/Image.lz4 $OUT_DIR
    cp $SHELL_DIR/$LINUX_DIR/arch/riscv/boot/dts/bouffalolab/bl808-pine64-ox64.dtb $OUT_DIR
    cd $OUT_DIR && ./mergebin.py
}

build_linux_config()
{
    echo " "
    echo "============ build linux kernel config ============="
    cd $SHELL_DIR/$LINUX_DIR
#    make ARCH=riscv CROSS_COMPILE=$LINUX_CROSS_PREFIX menuconfig -j$(nproc)
    make ARCH=riscv menuconfig -j$(nproc)
}

build_whole_bin()
{
    echo " "
    echo "================ build whole bin =================="
    cd $OUT_DIR
    python3 merge_7_5Mbin.py
}

build_all()
{
    build_linux
    build_whole_bin
}

clean_all()
{
    echo " "
    echo "================ clean out ================"
    find ./out ! -name 'squashfs_test.img' ! -name 'merge_7_5Mbin.py' -type f -exec rm -f {} +
    echo " "
    echo "================ clean kernel ================"
    cd $SHELL_DIR/$LINUX_DIR
    make ARCH=riscv CROSS_COMPILE=$LINUX_CROSS_PREFIX mrproper
    echo " "
    echo "================== clean opensbi ==================="
    cd $SHELL_DIR/opensbi-0.6-808
    make PLATFORM=thead/c910 CROSS_COMPILE=$LINUX_CROSS_PREFIX distclean

}

case "$BUILD_TARGET" in
--help)
    TARGET="kernel|kernel_config|opensbi|dtb|low_load|clean_load|whole_bin|all"
    USAGE="usage $0 [$TARGET]"
    echo $USAGE
    exit 0
    ;;

kernel)
    build_linux
    ;;
kernel_config)
    build_linux_config
    ;;
clean_load)
    build_clean_load
    ;;
whole_bin)
    build_whole_bin
    ;;
clean_all)
    clean_all
    ;;
all)
    build_all
    ;;
*)
    echo $USAGE
    exit 255
    ;;

esac

echo " "
echo "===================== build done ======================="

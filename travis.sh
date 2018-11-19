git clone https://github.com/ne0z/tizen_toolchain_kernel
export CROSS_COMPILE=./tizen_toolchain_kernel/bin/arm-linux-gnueabi- 
export ARCH=arm
export KBUILD_BUILD_USER="Qiangong2"
export KBUILD_BUILD_HOST="QG2ZHT1"
make tizen_z2_defconfig
make zImage

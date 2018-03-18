git clone https://github.com/Qiangong2/bluapprovedarmeabi4-8
export CROSS_COMPILE=./bluapprovedarmeabi4-8/arm-eabi-4.8/bin/arm-eabi- 
export ARCH=arm
export KBUILD_BUILD_USER="Qiangong2"
export KBUILD_BUILD_HOST="QG2ZHT1"
"ls -al"
make tizen_z2_defconfig
make zImage

#!/bin/sh

set -eu

CONFIG_FILE=magellan-c.config
LINUX_SRC=linux-3.4
KERN_VER=3.4.39-02-lobo
EXTRA_REV=-may0
DEB_REV=0

cd ${LINUX_SRC}

# mr proper
#make ARCH=arm distclean

# copy the config file
cp ../${CONFIG_FILE} ./.config

# update config
yes "" | make ARCH=arm oldconfig

# make a debian kernel package

export DEB_HOST_ARCH=armhf
export ARCH=arm

fakeroot make-kpkg -j 3 --initrd \
    --append-to-version=${EXTRA_REV} \
    --cross-compile arm-linux-gnueabihf- \
    --arch arm \
    --revision "${KERN_VER}${EXTRA_REV}-${DEB_REV}" \
    kernel-image kernel-headers

# also make a uboot-compatible kernel
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- EXTRAVERSION=${EXTRA_REV} uImage
cp arch/arm/boot/uImage ../

Magellan kernel - an Orange Pi 3.4 kernel
=========================================

This repository contains reciped and scripts to build the kernel
used on the Magellan board.

The old instructions and files have been moved to `old/`


How to build
------------


1. install a few dependencies:
   ```bash
   sudo apt-get install fakeroot kernel-package u-boot-tools gcc-arm-linux-gnueabihf
   ```

1. build the kernel debians & u-boot image:
   ```bash
   ./build.sh
   ```

This will generate 3 files:

- `linux-image-.....deb`: the kernel package (with modules)
- `linux-headers-.....deb`: the headers (to build more modules later)
- `uImage`: the u-boot compatible kernel (to be placed in the boot partition)

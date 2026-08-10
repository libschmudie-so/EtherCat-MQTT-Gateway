# CMake toolchain file for cross-compiling to armhf (32-bit ARM, hard-float)
# using Debian's crossbuild-essential-armhf. Used by Dockerfile.armhf.
#
# CMAKE_FIND_ROOT_PATH_MODE_PACKAGE is BOTH (not ONLY): nlohmann_json and
# cxxopts are header-only, architecture-independent CMake config packages
# installed at their normal host paths (via apt, arch:all) -- there's no
# compiled code to accidentally pick up the wrong architecture for, and
# restricting to the sysroot would just make them invisible.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf /opt/armhf-sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_PREFIX_PATH /opt/armhf-sysroot)

PROJECT(DRM_App)

CMAKE_MINIMUM_REQUIRED(VERSION 3.5)

# ===== Step 1 : Set Cross Compiler Path =====

SET(_SDK_PATH_ "/home/xjt/_Workspace_/System/")
SET(_BUILDROOT_PATH_ "${_SDK_PATH_}rk3588-linux/buildroot/output/rockchip_rk3588/host/")
SET(_COMPILER_PATH_ "${_BUILDROOT_PATH_}bin/")

SET(CMAKE_C_COMPILER ${_COMPILER_PATH_}aarch64-buildroot-linux-gnu-gcc)
SET(CMAKE_CXX_COMPILER ${_COMPILER_PATH_}aarch64-buildroot-linux-gnu-g++)

# ===== Step 2 : Set Flags =====

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -s -O3 -lrt -pthread")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s -O3 -lrt -pthread -std=c++20")

# ===== Step 3 : Include & Lib =====

SET(_INCLUDE_PATH_ "${_BUILDROOT_PATH_}/aarch64-buildroot-linux-gnu/sysroot/usr/include/")
SET(_LIB_PATH_ "${_BUILDROOT_PATH_}/aarch64-buildroot-linux-gnu/sysroot/usr/lib/")

LINK_DIRECTORIES(
    ${_LIB_PATH_}
)

INCLUDE_DIRECTORIES(
    ${_INCLUDE_PATH_}
)

# ===== Step 4 : Set Application Name =====

SET(APP_NAME "xdrm")

# ===== Step 5 : Add Subdirectory =====

ADD_SUBDIRECTORY(src bin)
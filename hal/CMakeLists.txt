# gnss_usb_hal — standalone AIDL GNSS HAL built with the Android NDK.
# The AIDL NDK-backend bindings under hal/generated are produced by
# build_hal.sh (aidl --lang=ndk) and are self-contained.

cmake_minimum_required(VERSION 3.18)
project(gnss_usb_hal CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shim headers must come first so <log/log.h>/<cutils/properties.h> resolve to
# the NDK-compatible replacements instead of missing AOSP platform headers.
include_directories(BEFORE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shim
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

file(GLOB_RECURSE AIDL_GENERATED_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generated/src/*.cpp)
file(GLOB HAL_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_executable(gnss_usb_hal
    ${HAL_SRCS}
    ${AIDL_GENERATED_SRCS}
)

target_link_libraries(gnss_usb_hal
    PRIVATE
    binder_ndk
    log
)

# Single-file deployment: static STL, no libc++_shared.so needed on device.
find_library(cxx_static c++_static)
if(cxx_static)
    target_link_libraries(gnss_usb_hal PRIVATE c++_static)
endif()

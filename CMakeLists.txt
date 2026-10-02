cmake_minimum_required(VERSION 3.14)
project (webos-vncserver C CXX)

# webOS NDK toolchain defaults to -Os, which makes compression and pixel
# conversion several times slower
add_compile_options(-O2)

add_subdirectory(prebuilt)
add_subdirectory(tv-native-apis)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/service)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/service/lib)
configure_file("${CMAKE_SOURCE_DIR}/services.json" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/services.json" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/autostart.sh" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/autostart.sh" COPYONLY)

# Copy required dependencies
find_library(JPEG_LIB NAMES libjpeg.so.8)
configure_file("${JPEG_LIB}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lib/libjpeg.so.8" COPYONLY)

find_library(SSL_LIB NAMES libssl.so.1.1)
configure_file("${SSL_LIB}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lib/libssl.so.1.1" COPYONLY)

find_library(CRYPTO_LIB NAMES libcrypto.so.1.1)
configure_file("${CRYPTO_LIB}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lib/libcrypto.so.1.1" COPYONLY)

find_library(SSL_LIB NAMES libatomic.so.1)
configure_file("${SSL_LIB}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lib/libatomic.so.1" COPYONLY)

find_package(PkgConfig REQUIRED)
include(FindPkgConfig)

pkg_check_modules(PBNJSON REQUIRED pbnjson_c)
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(LS2 REQUIRED luna-service2)
pkg_check_modules(PMLOG REQUIRED PmLogLib)

include_directories(${PBNJSON_INCLUDE_DIRS})
include_directories(${GLIB2_INCLUDE_DIRS})
include_directories(${LS2_INCLUDE_DIRS})
include_directories(${PMLOG_INCLUDE_DIRS})

set(CMAKE_INSTALL_RPATH $ORIGIN)
add_executable(webos-vncserver
    src/main.c
    src/service.c
    src/server.c
    src/settings.c
    src/uinput.c
    src/log.c
    src/compositor.c
    src/getauxval.c
)
set_property(TARGET webos-vncserver PROPERTY ENABLE_EXPORTS 1)
# libvncserver uses bundled zlib-ng and libdeflate, which are kept private to
# it - other libraries keep using system zlib
target_link_libraries(webos-vncserver im vncserver libdeflate zlibng pthread z png jpeg ssl crypto dl ${LS2_LDFLAGS} ${PBNJSON_LDFLAGS} ${GLIB2_LDFLAGS} ${PMLOG_LDFLAGS} rt m)
target_link_options(webos-vncserver PRIVATE "-Wl,--exclude-libs,libz.a:libdeflate.a")

set_target_properties(webos-vncserver PROPERTIES
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN:$ORIGIN/lib"
    BUILD_RPATH "$ORIGIN:$ORIGIN/lib"
)

add_library(capture_gm SHARED
    src/capture_gm.c
)
target_link_libraries(capture_gm gm)

add_library(capture_halgal SHARED
    src/capture_halgal.c
)
target_link_libraries(capture_halgal halgal)

add_library(capture_vtcapture SHARED
    src/capture_vtcapture.cpp
)
target_link_libraries(capture_vtcapture vtcapture)

add_library(capture_dile_vt SHARED
    src/capture_dile_vt.c
)
# libdile_vt.so is missing DT_NEEDED for libPmLogLib.so.3
target_link_libraries(capture_dile_vt dile_vt ${PMLOG_LDFLAGS})

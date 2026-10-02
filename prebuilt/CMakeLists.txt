include(ExternalProject)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

set(LIBVNCSERVER_PATCHES
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/0001-parallel-zlib-encoding.patch
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/0002-log-client-encodings.patch
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/0003-parallel-tight-jpeg.patch
)
string(REPLACE ";" " " LIBVNCSERVER_PATCHES "${LIBVNCSERVER_PATCHES}")

set(LIBDEFLATE_PATCHES
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/libdeflate-0001-sync-flush.patch
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/libdeflate-0002-pixel-mode.patch
)
string(REPLACE ";" " " LIBDEFLATE_PATCHES "${LIBDEFLATE_PATCHES}")

# libdeflate - faster deflate, used for zlib encoding (see 0001 patch) on CPUs
# with NEON, which it's built with. Patched to end its output with a sync
# flush, like zlib's Z_SYNC_FLUSH, and to compress pixels faster.
ExternalProject_Add(
  Libdeflate
  GIT_REPOSITORY https://github.com/ebiggers/libdeflate
  GIT_TAG v1.26
  PATCH_COMMAND sh -c "git apply --reverse --check ${LIBDEFLATE_PATCHES} 2>/dev/null || git apply ${LIBDEFLATE_PATCHES}"
  CMAKE_ARGS "-DLIBDEFLATE_BUILD_SHARED_LIB=OFF" "-DLIBDEFLATE_DECOMPRESSION_SUPPORT=OFF"
  "-DLIBDEFLATE_ZLIB_SUPPORT=OFF" "-DLIBDEFLATE_GZIP_SUPPORT=OFF" "-DLIBDEFLATE_BUILD_GZIP=OFF"
  "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_C_FLAGS=-mfpu=neon"
  "-DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/libdeflate"
  BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/libdeflate/lib/libdeflate.a
)

add_library(libdeflate STATIC IMPORTED GLOBAL)
set_target_properties(libdeflate PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/libdeflate/lib/libdeflate.a
)
add_dependencies(libdeflate Libdeflate)

# FIXME this seems fairly wrong - probably due to old cmake or some other
# Yocto/NDK-related bugs these need to be manually passed in :/
ExternalProject_Add(
  LibVNCServer
  GIT_REPOSITORY https://github.com/LibVNC/libvncserver
  GIT_TAG 0b8990784a81823eecc9089263ac1966d17bf399
  DEPENDS Libdeflate
  # Apply patches, unless they have been applied already
  PATCH_COMMAND sh -c "git apply --reverse --check ${LIBVNCSERVER_PATCHES} 2>/dev/null || git apply ${LIBVNCSERVER_PATCHES}"
  CMAKE_ARGS  "-DWITH_TESTS=OFF" "-DWITH_EXAMPLES=OFF" "-DBUILD_SHARED_LIBS=OFF" "-DWITH_SYSTEMD=OFF"
  # zlib encoding uses libdeflate and getauxval shim from src/
  "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -I${CMAKE_CURRENT_BINARY_DIR}/libdeflate/include -I${CMAKE_SOURCE_DIR}/src/shim"
  "-DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_SOURCE_DIR}"
  "-DJPEG_LIBRARY=${JPEG_LIBRARIES}" "-DJPEG_INCLUDE_DIR=${JPEG_INCLUDEDIR}"
  "-DZLIB_LIBRARY=${ZLIB_LIBRARIES}" "-DZLIB_INCLUDE_DIR=${ZLIB_INCLUDEDIR}"
)

add_library(vncserver STATIC IMPORTED GLOBAL)

set_target_properties(vncserver PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES      ${CMAKE_CURRENT_SOURCE_DIR}/include
    IMPORTED_LOCATION                  ${CMAKE_CURRENT_SOURCE_DIR}/lib/libvncserver.a
)

add_dependencies(vncserver LibVNCServer)

# zlib-ng - faster zlib implementation, used for libvncserver encodings. Runtime
# CPU feature detection (NEON) relies on getauxval shim from src/.
ExternalProject_Add(
  ZlibNg
  GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng
  GIT_TAG 2.2.4
  CMAKE_ARGS "-DZLIB_COMPAT=ON" "-DBUILD_SHARED_LIBS=OFF" "-DWITH_GZFILEOP=OFF"
  "-DZLIB_ENABLE_TESTS=OFF" "-DZLIBNG_ENABLE_TESTS=OFF" "-DWITH_GTEST=OFF"
  "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
  "-DCMAKE_C_FLAGS=-O3 -mtune=cortex-a76 -DHAVE_SYS_AUXV_H -DARM_AUXV_HAS_NEON -DARM_AUXV_HAS_CRC32 -I${CMAKE_SOURCE_DIR}/src/shim"
  "-DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/zlib-ng"
  BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/zlib-ng/lib/libz.a
)

add_library(zlibng STATIC IMPORTED GLOBAL)
set_target_properties(zlibng PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/zlib-ng/lib/libz.a
)
add_dependencies(zlibng ZlibNg)

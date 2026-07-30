# Undefined-behavior sanitizer toolchain. Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/undefined.cmake
# Compose with clang via CXX=clang++ (see address.cmake).
#
# TEMPLATE_UBSAN: GCC has no standard UBSan predefine before GCC 14's
# __has_feature. Defining it lets the sanitizer smoke test detect UBSan on
# GCC 13 as well; harmless on Clang / GCC 14+.
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

message(STATUS "undefined-behavior sanitizer: -fsanitize=undefined")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined -fno-omit-frame-pointer -DTEMPLATE_UBSAN")
set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=undefined")

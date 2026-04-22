# CMake toolchain file for cross-compiling ONNX Runtime to RV1106
#   target: armv7-a + NEON + VFPv4, uClibc-ng 1.0.50, gcc 8.3.0
#
# Used by build.sh via:
#   --cmake_extra_defines CMAKE_TOOLCHAIN_FILE=<this file>

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_ROOT /home/mt/AIGLASS_DEV_ENV/tools/linux/toolchain/arm-rockchip831-linux-uclibcgnueabihf)
set(TOOLCHAIN_PREFIX ${TOOLCHAIN_ROOT}/bin/arm-rockchip831-linux-uclibcgnueabihf-)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}ar            CACHE FILEPATH "ar")
set(CMAKE_RANLIB       ${TOOLCHAIN_PREFIX}ranlib        CACHE FILEPATH "ranlib")
set(CMAKE_STRIP        ${TOOLCHAIN_PREFIX}strip         CACHE FILEPATH "strip")
set(CMAKE_LINKER       ${TOOLCHAIN_PREFIX}ld            CACHE FILEPATH "ld")

# Match the ABI used by my-app (see my-app/Makefile: -mfpu=neon implied).
# RV1106 = Cortex-A7 -> armv7-a, NEON, hard-float (eabihf).
set(ARCH_FLAGS "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(SIZE_FLAGS "-ffunction-sections -fdata-sections")

# Build size > speed for ORT itself (we'll still get NEON via ARCH_FLAGS).
set(CMAKE_C_FLAGS_INIT   "${ARCH_FLAGS} ${SIZE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${ARCH_FLAGS} ${SIZE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-Wl,--gc-sections")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH ${TOOLCHAIN_ROOT}/arm-rockchip831-linux-uclibcgnueabihf/sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Silence a pile of warnings-as-errors that trip on gcc 8.3.
add_compile_options(-Wno-error -Wno-deprecated-declarations
                    -Wno-array-bounds -Wno-unused-result
                    -Wno-maybe-uninitialized)

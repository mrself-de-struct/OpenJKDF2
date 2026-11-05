set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(TOOLCHAIN_PREFIX i686-w64-mingw32)

# Keep pkg-config to the MinGW sysroot. Otherwise it reports the build
# machine's Linux packages (e.g. freeglut -> -I/usr/include, or PipeWire for
# OpenAL), and Linux headers end up in the Windows build. Applies to the
# bundled libraries' builds too, since they use this toolchain file.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/${TOOLCHAIN_PREFIX}/lib/pkgconfig:/usr/${TOOLCHAIN_PREFIX}/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}-gcc" CACHE STRING "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}-g++" CACHE STRING "C++ compiler" FORCE)
set(CMAKE_RC_COMPILER  "${TOOLCHAIN_PREFIX}-windres" CACHE STRING "rc compiler" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(WIN32 TRUE)
set(MINGW TRUE)
set(PLAT_HOOKS TRUE)

message( STATUS "MinGW 32-bit cross-compile toolchain invoked" )

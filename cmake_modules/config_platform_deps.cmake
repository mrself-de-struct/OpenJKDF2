include(ExternalProject)

# Makes cross-compiling easier in the build_whatever.cmake files
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${PROJECT_SOURCE_DIR}/cmake_modules/toolchain_native.cmake)
endif()

# Automatic updates
if(TARGET_USE_CURL)
    add_compile_definitions(
        HAVE_STRUCT_TIMEVAL
        HAVE_CONFIG_H
        BUILDING_LIBCURL
        USE_MBEDTLS
        __USE_MINGW_ANSI_STDIO
        CURL_STATICLIB
    )

    if(TARGET_LINUX)
        # Linux can just use the package manager version of libcurl, yay
        add_definitions(-DPLATFORM_CURL)
    else()
        # curl
        file(GLOB CURL_SRCS ${PROJECT_SOURCE_DIR}/src/external/curl/*.c ${PROJECT_SOURCE_DIR}/src/external/curl/vtls/*.c ${PROJECT_SOURCE_DIR}/src/external/curl/vauth/*.c  ${PROJECT_SOURCE_DIR}/src/external/curl/vquic/*.c)
        list(APPEND ENGINE_SOURCE_FILES ${CURL_SRCS})
        include_directories(${PROJECT_SOURCE_DIR}/src/external/curl)

        # mbedtls
        file(GLOB MBEDTLS_SRCS ${PROJECT_SOURCE_DIR}/src/external/mbedtls/*.c)
        list(APPEND ENGINE_SOURCE_FILES ${MBEDTLS_SRCS})
        include_directories(${PROJECT_SOURCE_DIR}/src/external/mbedtls)

        add_definitions(-DPLATFORM_CURL)
    endif()
endif()

if(TARGET_NO_BLOBS)
    add_definitions(-DLINUX_TMP)
    add_definitions(-DNO_JK_MMAP)
endif()

if (TARGET_NO_MULTIPLAYER_MENUS)
    add_definitions(-DTARGET_NO_MULTIPLAYER_MENUS)
endif()

if (TARGET_DROIDWORKS)
    add_compile_definitions(PLATFORM_DROIDWORKS)
endif()

# Enables all force powers by default, useful for debugging.
if(DEBUG_QOL_CHEATS)
    add_definitions(-DDEBUG_QOL_CHEATS)
endif()

# Enables Cxx compiling for fixed point templates
if(EXPERIMENTAL_FIXED_POINT)
    add_definitions(-DEXPERIMENTAL_FIXED_POINT)
endif()

find_package(GLUT)
if(NOT FreeGLUT_FOUND OR CMAKE_CROSSCOMPILING)
    message(STATUS "Going to build “FreeGLUT 3.4.0” from Git module")
    include(build_freeglut)
endif()

set(GLEW_USE_STATIC_LIBS TRUE)
if(NOT CMAKE_CROSSCOMPILING)
    find_package(GLEW 2.2.0)
endif()
# GLEW is a desktop-GL extension loader; the GLES targets (WASM, Android, iOS)
# get their entry points straight from the GLES library -- on iOS that library is
# ANGLE, and building GLEW for the iOS SDK fails outright (no GL/glx headers).
if((NOT GLEW_FOUND OR CMAKE_CROSSCOMPILING) AND NOT PLAT_WASM AND NOT TARGET_USE_OPENGL11 AND NOT TARGET_IOS)
    message(STATUS "Going to build “GLEW 2.2.0” from Git module")
    include(build_glew)
endif()

# Multiplayer uses DirectPlay, compatible with the original JK.EXE (see
# src/Platform/Networking/DPlay/README.md). Targets that can't run it get the
# stub backend (no multiplayer).
if(NOT TARGET_USE_DPLAY)
    set(TARGET_USE_NOSOCKETS TRUE)
endif()

message(STATUS "Going to build “zlib 1.2.13” from Git module")
include(build_zlib)

if(TARGET_USE_PHYSFS)
    message(STATUS "Going to build “PhysFS 3.2.0” from Git module")
    include(build_physfs)
    add_compile_definitions(PLATFORM_PHYSFS)
endif()

if(TARGET_USE_DPLAY)
    # Vanilla-compatible backend that talks to unmodified JK.EXE 1.0: the
    # DirectPlay 4 wire protocol over sockets (dplay_native.c; join and host),
    # or optionally Microsoft's DirectPlay on 32-bit Windows (dplay_shim.c).
    file(GLOB TARGET_DPLAY_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/Networking/DPlay/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_DPLAY_SRCS})
    # Hit what you see, from the browser build (Imperial Network Protocol and web lobby games only)
    file(GLOB TARGET_HITS_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/Networking/Hits/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_HITS_SRCS})

    add_compile_definitions(PLATFORM_DPLAY)
    # DirectPlay protocol implementation: our own (dplay_native.c) everywhere by
    # default. Microsoft's DirectPlay (dplay_shim.c, via COM) exists only as a
    # 32-bit component, so it can't work in 64-bit Windows builds; 32-bit
    # Windows builds can opt into it with -DDPLAY_MICROSOFT=ON.
    if(NOT (TARGET_WIN32 AND DPLAY_MICROSOFT))
        add_compile_definitions(DPLAY_NATIVE)
    endif()

    # Direct WebRTC links for the web lobby (needs libcurl for the lobby itself).
    # On by default on Linux (uses the system OpenSSL); opt in elsewhere with
    # -DTARGET_USE_WEBRTC=ON. Without it, web lobby games go through the lobby.
    if(NOT DEFINED TARGET_USE_WEBRTC)
        if(TARGET_USE_CURL AND TARGET_LINUX)
            set(TARGET_USE_WEBRTC TRUE)
        else()
            set(TARGET_USE_WEBRTC FALSE)
        endif()
    endif()
    if(TARGET_USE_WEBRTC)
        include(cmake_modules/build_libdatachannel.cmake)
    endif()
endif()

if(TARGET_USE_NOSOCKETS AND NOT TARGET_HOOKS)
    file(GLOB TARGET_NOSOCK_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/Networking/None/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_NOSOCK_SRCS})

    add_definitions(-DPLATFORM_NOSOCKETS)
endif()

message(STATUS "Going to build “libpng 1.6.39” from Git module")
include(build_libpng)

if(TARGET_USE_LIBSMACKER)
    list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/external/libsmacker/smacker.c ${PROJECT_SOURCE_DIR}/src/external/libsmacker/smk_bitstream.c ${PROJECT_SOURCE_DIR}/src/external/libsmacker/smk_hufftree.c)
endif()

if(TARGET_USE_LIBSMUSHER)
    list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/external/libsmusher/src/smush.c ${PROJECT_SOURCE_DIR}/src/external/libsmusher/src/codec48.c ${PROJECT_SOURCE_DIR}/src/external/libsmusher/src/codec47.c)
endif()

# Build SDL3 from sources (n/a for WASM)
if(TARGET_USE_SDL2 AND NOT PLAT_WASM)
    message(STATUS "Going to build “SDL 3.4.12” from Git module")
    include(build_sdl)

    if(TARGET_USE_OPENAL)
        message(STATUS "Going to build “SDL_mixer 3.2.4” from Git module")
        include(build_sdl_mixer)
    endif()
endif()

# SDL2 Platform/
if(TARGET_USE_SDL2)
    file(GLOB TARGET_SDL2_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/SDL2/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_SDL2_SRCS})
    add_compile_definitions(SDL2_RENDER)
endif()

if(TARGET_USE_OPENGL)
    file(GLOB TARGET_GL_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/GL/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_GL_SRCS})

    file(GLOB TARGET_GL_CPP_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/GL/*.cpp)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_GL_CPP_SRCS})
endif()

if(TARGET_USE_OPENGL11)
    file(GLOB TARGET_GL11_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/GL11/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_GL11_SRCS})
    add_compile_definitions(RENDER_GL11)

    # NOTE: jkQuakeConsole.c stays compiled here (unlike TWL). The shared SDL2 Window.c
    # references its symbols inside SDL2_RENDER blocks, and the GL11 backend stubs out
    # the UI text functions it uses (std3D_DrawUIBitmapRGBA/std3D_DrawUIClearedRectRGBA),
    # so the console links fine and simply renders nothing.
endif()

if((TARGET_USE_OPENAL AND NOT PLAT_WASM) OR TARGET_MACOS)
    #find_package(OpenAL 1.23.1)
    if(NOT OPENAL_FOUND)
        message(STATUS "Going to build “libopenal 1.23.1” from Git module")
        include(build_openal)
    else()
        message(STATUS "Using system OpenAL")
    endif()
    add_compile_definitions(STDSOUND_OPENAL)
elseif(PLAT_WASM)
    add_compile_definitions(STDSOUND_OPENAL)
elseif(TARGET_TWL)
    add_compile_definitions(STDSOUND_MAXMOD)
elseif(TARGET_DREAMCAST)
    add_compile_definitions(STDSOUND_DREAMCAST)
else()
    add_compile_definitions(STDSOUND_NULL)
endif()

if(TARGET_USE_D3D)
    file(GLOB TARGET_D3D_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/D3D/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_D3D_SRCS})
endif()

if(TARGET_POSIX)
    file(GLOB TARGET_POSIX_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/Posix/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_POSIX_SRCS})

    add_definitions(-DPLATFORM_POSIX)
endif()

if(TARGET_LINUX)
    list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/external/nativefiledialog-extended/nfd_gtk.cpp)

    add_definitions(-DLINUX)
    add_definitions(-DPLATFORM_LINUX)
    add_definitions(-DPLATFORM_NO_CACERT_BLOB)
endif()

if(TARGET_IOS)
    file(GLOB TARGET_IOS_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/iOS/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_IOS_SRCS})
    file(GLOB TARGET_IOS_M_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/iOS/*.m)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_IOS_M_SRCS})

    # No nativefiledialog-extended backend exists for iOS (nfd_cocoa.m is
    # NSOpenPanel/AppKit), so InstallHelper's folder-picker path is compiled out
    # there -- assets are dropped into the app's Documents dir instead.
endif()

if(TARGET_MACOS)
    list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/external/nativefiledialog-extended/nfd_cocoa.m)
    file(GLOB TARGET_MACOS_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/macOS/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_MACOS_SRCS})
    file(GLOB TARGET_MACOS_M_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/macOS/*.m)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_MACOS_M_SRCS})

    add_definitions(-DMACOS)
    add_definitions(-DLINUX)
endif()

if(TARGET_TWL)
    file(GLOB TARGET_TWL_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/TWL/*.c ${PROJECT_SOURCE_DIR}/src/Platform/TWL/mpu.s)
    set_property(SOURCE ${PROJECT_SOURCE_DIR}/src/Platform/TWL/mpu.s PROPERTY LANGUAGE C)
    set_property(SOURCE ${PROJECT_SOURCE_DIR}/src/Platform/TWL/mpu.s APPEND PROPERTY COMPILE_OPTIONS "-x" "assembler-with-cpp")
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_TWL_SRCS})
    list(REMOVE_ITEM ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/Main/jkQuakeConsole.c)
endif()

if(TARGET_DREAMCAST)
    file(GLOB TARGET_DREAMCAST_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/Dreamcast/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_DREAMCAST_SRCS})
    # Shared dlmalloc (ONLY_MSPACES) backs the tracked engine heap in stdPlatform.c.
    list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/Platform/TWL/dlmalloc.c)
    # No hardware-accelerated dev console (it relies on the SDL2/GL UI text path).
    list(REMOVE_ITEM ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/Main/jkQuakeConsole.c)
endif()

if(TARGET_WASM)
    add_definitions(-DLINUX)
endif()

if(TARGET_CAN_JKGM)
    add_definitions(-DTARGET_CAN_JKGM)
endif()

if(TARGET_WIN32)
    include_directories(
        ${PROJECT_SOURCE_DIR}/3rdparty/drmingw-0.9.3-win64/include
        ${PROJECT_SOURCE_DIR}/3rdparty/SDL3_mixer/x86_64-w64-mingw32/include/SDL3_mixer
    )
    file(GLOB TARGET_WIN32_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/Win32/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_WIN32_SRCS})

    add_subdirectory(${PROJECT_SOURCE_DIR}/packaging/win32)
    list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/packaging/win32/openjkdf2.rc)

    # Prefer the POSIX wuRegistry (JSON) over native
    if (TARGET_POSIX OR PLAT_MSVC)
        list(REMOVE_ITEM ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/Platform/Win32/wuRegistry.c)
    endif()

    if(PLAT_MSVC)
        list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/Platform/Posix/wuRegistry.c)
    endif()

    if(TARGET_USE_SDL2)
        list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/external/nativefiledialog-extended/nfd_win.cpp)
        if(PLAT_MSVC)
            set(LINK_LIBS ${LINK_LIBS} ole32.lib uuid.lib)
        endif()
    endif()
endif()


file(GLOB EMBEDDED_RESOURCES ${PROJECT_SOURCE_DIR}/resource/ui/*)
if (TARGET_USE_SDL2)
    file(GLOB SDL2_EMBEDDED_RES ${PROJECT_SOURCE_DIR}/resource/shaders/*)
    list(APPEND EMBEDDED_RESOURCES ${SDL2_EMBEDDED_RES})
endif()

if (TARGET_USE_CURL)
    file(GLOB CURL_EMBEDDED_RES ${PROJECT_SOURCE_DIR}/resource/ssl/*)
    list(APPEND EMBEDDED_RESOURCES ${CURL_EMBEDDED_RES})
endif()
